#include "dev_ap6212_bt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#include <linux/tty.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* N_HCI line discipline (部分内核头文件可能不定义) */
#ifndef N_HCI
#define N_HCI 15
#endif

/* HCIUARTSETPROTO ioctl (部分系统无 linux/hci_uart.h) */
#ifndef HCIUARTSETPROTO
#include <asm/ioctls.h>
#define HCIUARTSETPROTO _IOW('U', 200, int)
#endif
#ifndef HCI_UART_H4
#define HCI_UART_H4 0
#endif

/* HCI 设备 ID (hci0) */
static int hci_dev_id = -1;
static int hci_sock = -1;

/* 直接固件加载时保持 UART fd 打开 (N_HCI 需要) */
static int hci_uart_fd = -1;

/* ============================================================
 * 内部辅助函数
 * ============================================================ */

/**
 * @brief 执行系统命令
 */
static int execute_cmd(const char *cmd)
{
    int ret = system(cmd);
    if (ret != 0)
    {
        printf("[BT] Command failed: %s\n", cmd);
    }
    return ret;
}

/**
 * @brief 从 LE Advertising Report 中解析设备名称
 *
 * 在广播数据(EIR)中搜索 Complete/Shortened Local Name
 */
static void parse_ble_name(const uint8_t *eir, size_t eir_len, char *name, size_t name_len)
{
    name[0] = '\0';
    size_t offset = 0;

    while (offset < eir_len)
    {
        uint8_t field_len = eir[offset];
        if (field_len == 0 || offset + field_len >= eir_len)
            break;

        uint8_t field_type = eir[offset + 1];

        /* 0x09 = Complete Local Name, 0x08 = Shortened Local Name */
        if (field_type == 0x09 || field_type == 0x08)
        {
            size_t copy_len = field_len - 1;
            if (copy_len >= name_len)
                copy_len = name_len - 1;
            memcpy(name, &eir[offset + 2], copy_len);
            name[copy_len] = '\0';
            return;
        }

        offset += field_len + 1;
    }
}

/**
 * @brief 查找蓝牙 rfkill 设备并写入值
 * @param value "1" = block (断电), "0" = unblock (上电)
 * @return 0 成功, -1 失败
 */
static int rfkill_set_bluetooth(const char *value)
{
    DIR *dir = opendir("/sys/class/rfkill");
    if (!dir)
    {
        printf("[BT] Cannot open /sys/class/rfkill\n");
        return -1;
    }

    struct dirent *entry;
    char path[256];
    char type[32];
    int found = 0;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strncmp(entry->d_name, "rfkill", 6) != 0)
            continue;

        /* 读取 type 文件判断是否为蓝牙 */
        snprintf(path, sizeof(path), "/sys/class/rfkill/%s/type", entry->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            continue;

        int n = read(fd, type, sizeof(type) - 1);
        close(fd);
        if (n <= 0)
            continue;

        type[n] = '\0';
        /* 去除末尾换行 */
        if (n > 0 && type[n - 1] == '\n')
            type[n - 1] = '\0';

        if (strcmp(type, "bluetooth") != 0)
            continue;

        /* 找到蓝牙 rfkill 设备，写入 soft */
        printf("[BT] Found bluetooth rfkill: %s\n", entry->d_name);
        snprintf(path, sizeof(path), "/sys/class/rfkill/%s/soft", entry->d_name);
        fd = open(path, O_WRONLY);
        if (fd < 0)
        {
            printf("[BT] Cannot open %s: %s\n", path, strerror(errno));
            continue;
        }

        if (write(fd, value, strlen(value)) < 0)
        {
            printf("[BT] Failed to write %s to %s: %s\n", value, path, strerror(errno));
            close(fd);
            continue;
        }

        close(fd);
        printf("[BT] rfkill %s = %s\n", entry->d_name, value);
        found = 1;
        break;
    }

    closedir(dir);
    return found ? 0 : -1;
}

/**
 * @brief 通过 rfkill 对蓝牙芯片执行完整的断电→上电循环
 *
 * 内核的 wireless-bluetooth 驱动管理 BT_REG_ON GPIO。
 * rfkill block = 拉低 BT_REG_ON (断电)
 * rfkill unblock = 拉高 BT_REG_ON (上电)
 * 直接操作 GPIO sysfs 不可行 (内核驱动已占用)。
 */
static void bt_power_cycle(void)
{
    printf("[BT] Power cycling chip via rfkill...\n");

    /* 断电: rfkill block (sysfs + command) */
    rfkill_set_bluetooth("1");
    system("rfkill block bluetooth 2>/dev/null");
    printf("[BT] Chip powered OFF, waiting 3s...\n");
    sleep(3);

    /* 上电: rfkill unblock (sysfs + command) */
    rfkill_set_bluetooth("0");
    system("rfkill unblock bluetooth 2>/dev/null");
    printf("[BT] Chip powered ON, waiting 3s for ready...\n");
    sleep(3);
}

/**
 * @brief 配置 UART 串口参数
 *
 * 在 brcm_patchram_plus1 / hciattach 运行前调用,
 * 确保 UART 处于正确的初始状态.
 * BCM43430A1 芯片上电后默认 115200 8N1, 无硬件流控.
 */
static void setup_uart(const char *uart_dev, int init_baudrate)
{
    int fd = open(uart_dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        printf("[BT] Cannot open %s for UART setup: %s\n",
               uart_dev, strerror(errno));
        return;
    }

    /* 清除 O_NONBLOCK */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    /* 重置 line discipline */
    int ldisc = N_TTY;
    ioctl(fd, TIOCSETD, &ldisc);

    /* 配置 termios */
    struct termios ti;
    tcgetattr(fd, &ti);
    cfmakeraw(&ti);
    ti.c_cflag |= CLOCAL;   /* 忽略 modem 状态 */
    ti.c_cflag &= ~CRTSCTS; /* 关闭硬件流控 (芯片无固件时不支持) */
    ti.c_cflag &= ~CSTOPB;  /* 1 个停止位 */
    ti.c_cflag &= ~CSIZE;
    ti.c_cflag |= CS8;                     /* 8 位数据 */
    ti.c_cflag &= ~PARENB;                 /* 无校验 */
    ti.c_iflag &= ~(IXON | IXOFF | IXANY); /* 关闭软件流控 */
    ti.c_cc[VMIN] = 0;
    ti.c_cc[VTIME] = 0;

    speed_t speed;
    switch (init_baudrate)
    {
    case 9600:
        speed = B9600;
        break;
    case 19200:
        speed = B19200;
        break;
    case 38400:
        speed = B38400;
        break;
    case 57600:
        speed = B57600;
        break;
    case 115200:
        speed = B115200;
        break;
    case 230400:
        speed = B230400;
        break;
    case 460800:
        speed = B460800;
        break;
    case 921600:
        speed = B921600;
        break;
    case 1500000:
        speed = B1500000;
        break;
    default:
        speed = B115200;
        break;
    }
    cfsetospeed(&ti, speed);
    cfsetispeed(&ti, speed);

    tcflush(fd, TCIOFLUSH);
    tcsetattr(fd, TCSANOW, &ti);
    tcflush(fd, TCIOFLUSH);

    printf("[BT] UART %s configured: %d baud, 8N1, no flow control\n",
           uart_dev, init_baudrate);

    close(fd);
}

/* ============================================================
 * 直接 HCI 固件加载器
 *
 * 目标系统上的 brcm_patchram_plus1 为 AMPAK 定制版,
 * 在 HCI_Reset 后发 HCI_Read_Local_Name 做芯片自动检测,
 * 但 BCM43430A1 bootloader 不支持该命令, 导致超时退出.
 *
 * 本实现参照原版 Broadcom/Rockchip BSP 源码的加载顺序:
 *   proc_reset → proc_baudrate → proc_patchram → proc_enable_hci
 * 完全绕过 AMPAK 的 Read_Local_Name 步骤.
 * ============================================================ */

/**
 * @brief 从 UART 读取一个完整的 HCI 事件
 *
 * HCI event 格式: 04 <event_code> <param_total_len> <params...>
 * 4 字节指示符 + 2 字节头 + N 字节参数
 */
static int hci_uart_read_event(int fd, uint8_t *buf, int max_len, int timeout_ms)
{
    int total = 0;

    /* 读取事件指示符 (0x04), 跳过残留垃圾字节 */
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int found_indicator = 0;
    for (int skip = 0; skip < 32; skip++)
    {
        if (poll(&pfd, 1, (skip == 0) ? timeout_ms : 200) <= 0)
            return -1;
        if (read(fd, &buf[0], 1) != 1)
            return -1;
        if (buf[0] == 0x04)
        {
            found_indicator = 1;
            break;
        }
    }
    if (!found_indicator)
        return -1;

    /* 读取 event_code + param_len (2 字节) */
    total = 0;
    while (total < 2)
    {
        if (poll(&pfd, 1, 1000) <= 0)
            return -1;
        int n = read(fd, &buf[1 + total], 2 - total);
        if (n <= 0)
            return -1;
        total += n;
    }

    int param_len = buf[2];
    if (param_len > max_len - 3)
        param_len = max_len - 3;

    /* 读取参数 */
    total = 0;
    while (total < param_len)
    {
        if (poll(&pfd, 1, 1000) <= 0)
            return -1;
        int n = read(fd, &buf[3 + total], param_len - total);
        if (n <= 0)
            return -1;
        total += n;
    }

    return 3 + param_len;
}

/**
 * @brief 发送 HCI 命令并等待 Command Complete 事件
 */
static int hci_uart_send_cmd(int fd, const uint8_t *cmd, int cmd_len, int timeout_ms)
{
    if (write(fd, cmd, cmd_len) != cmd_len)
        return -1;

    uint8_t evt[260];
    int len = hci_uart_read_event(fd, evt, sizeof(evt), timeout_ms);
    if (len < 0)
        return -1;

    /* evt[1] = event code: 0x0E = Command Complete */
    if (evt[1] == 0x0E && len >= 6)
    {
        /* evt[6] = status (如果有) */
        return (len > 6) ? evt[6] : 0;
    }

    return 0; /* 接受其他事件 */
}

/**
 * @brief 直接通过 UART 加载 BCM HCI 固件 (.hcd 文件)
 *
 * 流程 (参照 BSP brcm_patchram_plus1.c):
 *   1. 打开 UART, 配置 115200 8N1
 *   2. proc_reset: HCI_Reset
 *   3. proc_patchram: Download Minidriver + .hcd 记录
 *   4. proc_baudrate: 切换到 1500000 波特率
 *   5. proc_enable_hci: 设置 N_HCI line discipline
 *
 * @return 0 成功, -1 失败
 */
static int bt_load_firmware_direct(const char *uart_dev, const char *fw_path)
{
    printf("[BT] Direct firmware loader: %s → %s\n", fw_path, uart_dev);

    /* 1. 打开 UART — 完全按照 BSP brcm_patchram_plus1.c init_uart() */
    int fd = open(uart_dev, O_RDWR | O_NOCTTY);
    if (fd < 0)
    {
        printf("[BT] Cannot open %s: %s\n", uart_dev, strerror(errno));
        return -1;
    }

    /* 重置 line discipline 为 N_TTY (防止上一次遗留 N_HCI) */
    int ldisc_tty = N_TTY;
    ioctl(fd, TIOCSETD, &ldisc_tty);

    /* BSP init_uart() 的精确复制:
     *   tcflush → cfmakeraw → CRTSCTS → tcsetattr×2 → tcflush×3 → baudrate → tcsetattr
     */
    struct termios ti;
    tcflush(fd, TCIOFLUSH);
    tcgetattr(fd, &ti);
    cfmakeraw(&ti);
    ti.c_cflag |= CRTSCTS; /* BSP 第629行: 开启硬件流控! */
    tcsetattr(fd, TCSANOW, &ti);
    tcflush(fd, TCIOFLUSH);
    tcsetattr(fd, TCSANOW, &ti);
    tcflush(fd, TCIOFLUSH);
    tcflush(fd, TCIOFLUSH);
    cfsetospeed(&ti, B115200);
    cfsetispeed(&ti, B115200);
    tcsetattr(fd, TCSANOW, &ti);

    printf("[BT] UART configured: 115200, 8N1, CRTSCTS (matching BSP)\n");

    /* 检查 modem 控制信号 (诊断) */
    int modem_bits = 0;
    if (ioctl(fd, TIOCMGET, &modem_bits) == 0)
    {
        printf("[BT] Modem signals: CTS=%d DSR=%d DCD=%d RTS=%d DTR=%d\n",
               !!(modem_bits & TIOCM_CTS),
               !!(modem_bits & TIOCM_DSR),
               !!(modem_bits & TIOCM_CAR),
               !!(modem_bits & TIOCM_RTS),
               !!(modem_bits & TIOCM_DTR));
    }

    /* 2. proc_reset: HCI_Reset
     * 尝试两种波特率:
     *   - 115200: 芯片刚上电/正常 bootloader 状态
     *   - 1500000: 如果上一次已加载固件并切换了波特率
     */
    printf("[BT] HCI_Reset...\n");
    uint8_t hci_reset[] = {0x01, 0x03, 0x0C, 0x00};
    int ret = -1;
    int current_baud = 115200;

    for (int attempt = 0; attempt < 6; attempt++)
    {
        if (attempt == 3)
        {
            /* 前3次 115200 失败后, 尝试 1500000
             * (芯片可能从上一次固件加载后还在高速模式) */
            printf("[BT] Trying 1500000 baud (chip may have firmware from previous run)...\n");
            cfsetospeed(&ti, B1500000);
            cfsetispeed(&ti, B1500000);
            tcsetattr(fd, TCSANOW, &ti);
            tcflush(fd, TCIOFLUSH);
            current_baud = 1500000;
        }
        else if (attempt > 0)
        {
            printf("[BT] HCI_Reset retry %d (baud=%d)...\n", attempt + 1, current_baud);
            usleep(500000);
            tcflush(fd, TCIOFLUSH);
        }

        /* 发送 HCI_Reset */
        int wret = write(fd, hci_reset, sizeof(hci_reset));
        if (wret != (int)sizeof(hci_reset))
        {
            printf("[BT] write() returned %d (errno=%d: %s)\n", wret, errno, strerror(errno));
            continue;
        }
        tcdrain(fd); /* 等待数据物理发送完毕 */

        /* 等待响应 */
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int poll_ret = poll(&pfd, 1, 4000);
        if (poll_ret <= 0)
        {
            printf("[BT] No response (poll=%d, baud=%d, attempt %d)\n",
                   poll_ret, current_baud, attempt + 1);

            /* 再次检查 modem 信号 */
            if (ioctl(fd, TIOCMGET, &modem_bits) == 0)
            {
                printf("[BT]   CTS=%d RTS=%d\n",
                       !!(modem_bits & TIOCM_CTS), !!(modem_bits & TIOCM_RTS));
            }
            continue;
        }

        /* 读取所有可用数据 */
        uint8_t raw[64];
        usleep(100000);
        int nread = read(fd, raw, sizeof(raw));
        if (nread <= 0)
        {
            printf("[BT] read() returned %d\n", nread);
            continue;
        }

        printf("[BT] Received %d bytes:", nread);
        for (int i = 0; i < nread && i < 32; i++)
            printf(" %02X", raw[i]);
        printf("\n");

        /* 在数据中搜索 HCI Command Complete for Reset (04 0E 04 01 03 0C 00) */
        int found = 0;
        for (int i = 0; i < nread - 3; i++)
        {
            if (raw[i] == 0x04 && raw[i + 1] == 0x0E)
            {
                int plen = raw[i + 2];
                if (i + 3 + plen <= nread && plen >= 3)
                {
                    uint16_t opcode = raw[i + 4] | (raw[i + 5] << 8);
                    if (opcode == 0x0C03)
                    {
                        ret = raw[i + 6];
                        found = 1;
                        break;
                    }
                }
            }
        }

        if (found && ret == 0)
            break;
        if (found)
            printf("[BT] HCI_Reset returned status %d\n", ret);
        else
            printf("[BT] Unexpected response (not HCI_Reset Complete)\n");
    }

    if (ret != 0)
    {
        printf("[BT] HCI_Reset failed after all attempts\n");

        /* 最后诊断: 检查 UART 是否可以给自己发数据 (回环测试) */
        printf("[BT] Diagnostics: checking UART TX/RX...\n");

        /* 关闭硬件流控尝试一次 */
        ti.c_cflag &= ~CRTSCTS;
        tcsetattr(fd, TCSANOW, &ti);
        cfsetospeed(&ti, B115200);
        cfsetispeed(&ti, B115200);
        tcsetattr(fd, TCSANOW, &ti);
        tcflush(fd, TCIOFLUSH);

        printf("[BT] Retrying without CRTSCTS...\n");
        int wret = write(fd, hci_reset, sizeof(hci_reset));
        tcdrain(fd);
        printf("[BT]   write=%d\n", wret);

        struct pollfd pfd2 = {.fd = fd, .events = POLLIN};
        int poll_ret2 = poll(&pfd2, 1, 3000);
        printf("[BT]   poll=%d\n", poll_ret2);
        if (poll_ret2 > 0)
        {
            uint8_t raw2[64];
            usleep(100000);
            int n2 = read(fd, raw2, sizeof(raw2));
            printf("[BT]   read=%d bytes:", n2);
            for (int i = 0; i < n2 && i < 32; i++)
                printf(" %02X", raw2[i]);
            printf("\n");

            /* 如果无流控也收到了 Command Complete, 继续加载 */
            for (int i = 0; i < n2 - 6; i++)
            {
                if (raw2[i] == 0x04 && raw2[i + 1] == 0x0E)
                {
                    int plen = raw2[i + 2];
                    if (i + 3 + plen <= n2 && plen >= 3)
                    {
                        uint16_t opcode = raw2[i + 4] | (raw2[i + 5] << 8);
                        if (opcode == 0x0C03 && raw2[i + 6] == 0)
                        {
                            printf("[BT] HCI_Reset OK without CRTSCTS!\n");
                            ret = 0;
                            current_baud = 115200;
                            /* 继续不带 CRTSCTS */
                        }
                    }
                }
            }
        }

        if (ret != 0)
        {
            printf("[BT] HCI_Reset completely failed\n");
            close(fd);
            return -1;
        }
    }
    else
    {
        printf("[BT] HCI_Reset OK (baud=%d)\n", current_baud);
    }

    /* 如果芯片在 1500000 响应了, 说明固件已经可用, 直接跳到 N_HCI 设置 */
    if (current_baud == 1500000)
    {
        printf("[BT] Chip already running with firmware at 1500000\n");
        printf("[BT] Skipping firmware reload, proceeding to N_HCI setup\n");

        /* 确保 UART 配置正确 (1500000 + CRTSCTS) */
        cfsetospeed(&ti, B1500000);
        cfsetispeed(&ti, B1500000);
        tcsetattr(fd, TCSANOW, &ti);
        tcflush(fd, TCIOFLUSH);

        /* 直接跳到设置 N_HCI line discipline */
        goto setup_nhci;
    }

    /* 3. proc_patchram: 下载固件 */
    printf("[BT] Loading firmware...\n");

    /* 3a. Download Minidriver */
    uint8_t hci_dl_minidrv[] = {0x01, 0x2E, 0xFC, 0x00};
    ret = hci_uart_send_cmd(fd, hci_dl_minidrv, sizeof(hci_dl_minidrv), 4000);
    if (ret < 0)
    {
        printf("[BT] Download Minidriver command failed\n");
        close(fd);
        return -1;
    }

    /* --no2bytes: 跳过 2 字节 (部分芯片固件头的尾部) */
    /* BSP 源码: if (!no2bytes) { read(uart_fd, &buffer[0], 2); } */
    /* 我们默认使用 --no2bytes, 所以不读 */

    /* --tosleep: 固件下载前等 200ms */
    usleep(200000);

    /* 3b. 打开 .hcd 固件文件 */
    int hcd_fd = open(fw_path, O_RDONLY);
    if (hcd_fd < 0)
    {
        printf("[BT] Cannot open firmware: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* 获取文件大小 */
    off_t fw_size = lseek(hcd_fd, 0, SEEK_END);
    lseek(hcd_fd, 0, SEEK_SET);
    printf("[BT] Firmware: %ld bytes\n", (long)fw_size);

    /* 3c. 逐条发送 HCD 记录 */
    int cmd_count = 0;
    int err_count = 0;
    uint8_t hcd_buf[260];

    while (1)
    {
        /* HCD 记录格式: [opcode_lo] [opcode_hi] [param_len] [params...] */
        /* 读取 3 字节头 */
        if (read(hcd_fd, &hcd_buf[1], 3) != 3)
            break; /* EOF 或读取错误 */

        int plen = hcd_buf[3];

        /* 读取参数 */
        if (plen > 0)
        {
            if (read(hcd_fd, &hcd_buf[4], plen) != plen)
                break;
        }

        /* 添加 HCI 命令包指示符 */
        hcd_buf[0] = 0x01;

        /* 发送并等待响应 */
        if (write(fd, hcd_buf, plen + 4) != plen + 4)
        {
            err_count++;
            break;
        }

        uint8_t evt[260];
        int elen = hci_uart_read_event(fd, evt, sizeof(evt), 2000);
        if (elen < 0)
            err_count++;

        cmd_count++;

        if (cmd_count % 100 == 0)
        {
            off_t pos = lseek(hcd_fd, 0, SEEK_CUR);
            printf("[BT]   %d cmds, %ld/%ld bytes\n", cmd_count, (long)pos, (long)fw_size);
        }
    }

    close(hcd_fd);
    printf("[BT] Firmware sent: %d commands, %d errors\n", cmd_count, err_count);

    if (cmd_count == 0)
    {
        printf("[BT] No firmware commands sent, aborting\n");
        close(fd);
        return -1;
    }

    /* 芯片执行固件, 等 200ms (BSP: usleep(200000)) */
    usleep(200000);

    /* proc_patchram 结束后做一次 proc_reset */
    printf("[BT] Post-firmware HCI_Reset...\n");
    tcflush(fd, TCIOFLUSH);
    ret = hci_uart_send_cmd(fd, hci_reset, sizeof(hci_reset), 4000);
    if (ret < 0)
    {
        printf("[BT] Post-firmware HCI_Reset failed, retrying...\n");
        usleep(500000);
        tcflush(fd, TCIOFLUSH);
        ret = hci_uart_send_cmd(fd, hci_reset, sizeof(hci_reset), 4000);
        if (ret < 0)
        {
            printf("[BT] Post-firmware HCI_Reset still failed\n");
            close(fd);
            return -1;
        }
    }
    printf("[BT] Post-firmware HCI_Reset OK\n");

    /* 4. proc_baudrate: 切换到 1500000 */
    /* HCI_Update_Baudrate (vendor cmd 0xFC18):
     * params: 00 00 [baudrate LE 4 bytes]
     * 1500000 = 0x0016E360 → 60 E3 16 00 */
    uint8_t hci_baud[] = {0x01, 0x18, 0xFC, 0x06,
                          0x00, 0x00, 0x60, 0xE3, 0x16, 0x00};
    printf("[BT] Switching to 1500000 baud...\n");
    ret = hci_uart_send_cmd(fd, hci_baud, sizeof(hci_baud), 2000);
    if (ret >= 0)
    {
        usleep(200000);
        cfsetospeed(&ti, B1500000);
        cfsetispeed(&ti, B1500000);
        tcsetattr(fd, TCSANOW, &ti);
        printf("[BT] Baudrate switched to 1500000\n");

        /* 验证: 在新波特率上 HCI_Reset */
        usleep(100000);
        tcflush(fd, TCIOFLUSH);
        ret = hci_uart_send_cmd(fd, hci_reset, sizeof(hci_reset), 2000);
        if (ret < 0)
        {
            printf("[BT] HCI at 1500000 failed, reverting to 115200\n");
            cfsetospeed(&ti, B115200);
            cfsetispeed(&ti, B115200);
            tcsetattr(fd, TCSANOW, &ti);
        }
    }
    else
    {
        printf("[BT] Baudrate change failed, staying at 115200\n");
    }

setup_nhci:
    /* 5. proc_enable_hci: 设置 N_HCI line discipline + HCI UART 协议 */
    printf("[BT] Setting N_HCI line discipline...\n");
    int ldisc = N_HCI;
    if (ioctl(fd, TIOCSETD, &ldisc) < 0)
    {
        printf("[BT] Failed to set N_HCI: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    /* 设置 HCI UART 协议为 H4 — 这一步触发内核创建 hci0 */
    int proto = HCI_UART_H4;
    if (ioctl(fd, HCIUARTSETPROTO, &proto) < 0)
    {
        printf("[BT] Failed to set HCI UART proto: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    printf("[BT] HCI UART protocol set (H4)\n");

    /* 保持 fd 打开 (关闭会导致 hci0 消失) */
    hci_uart_fd = fd;

    printf("[BT] Direct firmware load complete\n");
    return 0;
}

/* ============================================================
 * 公有接口
 * ============================================================ */

int dev_ap6212_bt_init(const char *uart_dev, int baudrate)
{
    char cmd[512];

    printf("\n=== AP6212 Bluetooth Init (BCM43430A1) ===\n");
    printf("UART: %s, Baudrate: %d\n\n", uart_dev, baudrate);

    /* ===== Phase 0: 尝试复用已有的 hci0 ===== */
    printf("[0/7] Checking for existing HCI device...\n");

    /* 场景1: hci0 已存在 (上一次 brcm_patchram_plus1 仍在运行) */
    if (access("/sys/class/bluetooth/hci0", F_OK) == 0)
    {
        system("hciconfig hci0 up 2>/dev/null");
        usleep(500000);

        hci_dev_id = hci_get_route(NULL);
        if (hci_dev_id >= 0)
        {
            hci_sock = hci_open_dev(hci_dev_id);
            if (hci_sock >= 0)
            {
                struct hci_dev_info di;
                di.dev_id = hci_dev_id;
                if (ioctl(hci_sock, HCIGETDEVINFO, (void *)&di) == 0 &&
                    hci_test_bit(HCI_UP, &di.flags))
                {
                    printf("[BT] Reusing existing hci%d\n", hci_dev_id);
                    system("hciconfig hci0");
                    printf("\n[BT] Init success (reused hci%d)\n\n", hci_dev_id);
                    return 0;
                }
                hci_close_dev(hci_sock);
                hci_sock = -1;
            }
        }
        printf("[BT] hci0 exists but not functional\n");
    }

    /* 场景2: brcm_patchram_plus1 或 hciattach 正在运行但 hci0 尚未创建 */
    if (system("ps | grep -q '[b]rcm_patchram_plus1'") == 0 ||
        system("ps | grep -q '[h]ciattach'") == 0)
    {
        printf("[BT] BT daemon running, waiting for hci0...\n");
        for (int w = 0; w < 15; w++)
        {
            sleep(1);
            if (access("/sys/class/bluetooth/hci0", F_OK) == 0)
            {
                system("hciconfig hci0 up 2>/dev/null");
                usleep(500000);
                hci_dev_id = hci_get_route(NULL);
                if (hci_dev_id >= 0)
                {
                    hci_sock = hci_open_dev(hci_dev_id);
                    if (hci_sock >= 0)
                    {
                        printf("[BT] hci%d ready after wait\n", hci_dev_id);
                        system("hciconfig hci0");
                        printf("\n[BT] Init success (reused hci%d)\n\n", hci_dev_id);
                        return 0;
                    }
                }
                break;
            }
        }
        printf("[BT] brcm_patchram_plus1 running but hci0 unavailable\n");
    }
    else
    {
        printf("[BT] No existing BT service, fresh init needed\n");
    }

    /* ===== 1. 清理现有进程和 UART ===== */
    printf("[1/7] Cleaning up...\n");

    if (hci_sock >= 0)
    {
        hci_close_dev(hci_sock);
        hci_sock = -1;
        hci_dev_id = -1;
    }

    execute_cmd("hciconfig hci0 down 2>/dev/null");
    usleep(200000);
    execute_cmd("killall -9 brcm_patchram_plus1 2>/dev/null");
    execute_cmd("killall -9 hciattach 2>/dev/null");
    sleep(1);

    /* 关闭可能遗留的直接固件加载 UART fd */
    if (hci_uart_fd >= 0)
    {
        close(hci_uart_fd);
        hci_uart_fd = -1;
    }

    /* ===== 2. 创建固件符号链接 (AMPAK 自动检测回退) ===== */
    printf("[2/7] Creating firmware symlinks for AMPAK auto-detection...\n");
    {
        /*
         * 当芯片已被加载过固件时, HCI_Read_Local_Name 返回完整名称:
         *   "BCM43438A1 26MHZ AP6212A1_CL1 BT4.0 OTP-BD"
         * AMPAK 自动检测会将其构造为错误的固件路径.
         * 创建符号链接确保无论芯片返回什么名称, 固件都能被找到.
         */
        const char *fw_src = "/system/etc/firmware/BCM43430A1.hcd";
        const char *link_dir = "/system/etc/firmware/4.0 OTP-BD";
        const char *link_path = "/system/etc/firmware/4.0 OTP-BD/"
                                "BCM43438A1 26MHZ AP6212A1_CL1 BT4.0 OTP-BD.hcd";
        const char *link_lc_dir = "/system/etc/firmware/4.0 otp-bd";
        const char *link_lc_path = "/system/etc/firmware/4.0 otp-bd/"
                                   "bcm43438a1 26mhz ap6212a1_cl1 bt4.0 otp-bd.hcd";

        /* 创建目录和符号链接 (大写) */
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", link_dir);
        system(cmd);
        if (access(link_path, F_OK) != 0)
        {
            if (symlink(fw_src, link_path) == 0)
                printf("[BT] Created firmware symlink (uppercase)\n");
        }

        /* 创建目录和符号链接 (小写) */
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s' 2>/dev/null", link_lc_dir);
        system(cmd);
        if (access(link_lc_path, F_OK) != 0)
        {
            if (symlink(fw_src, link_lc_path) == 0)
                printf("[BT] Created firmware symlink (lowercase)\n");
        }
    }

    /* ===== 3. 通过 rfkill 硬件复位蓝牙芯片 ===== */
    printf("[3/7] Resetting Bluetooth chip via rfkill...\n");
    bt_power_cycle();

    /* ===== 4. 加载固件 ===== */

    int hci_ready = 0;
    const char *fw_path = "/system/etc/firmware/BCM43430A1.hcd";

    /*
     * 初始化策略 (按优先级):
     *
     * 方法 A: 直接 HCI 固件加载
     *   参照原版 BSP 源码: init_uart → proc_reset → proc_patchram → proc_baudrate → proc_enable_hci
     *   关键: UART 配置精确匹配 BSP (包括 CRTSCTS 硬件流控)
     *
     * 方法 B: brcm_patchram_plus1 (兼容回退)
     * 方法 C: hciattach bcm43xx (BlueZ 回退)
     */

    /* ---------- 方法 A: 直接固件加载 ---------- */
    printf("[4/7] Loading firmware (direct method)...\n");

    if (access(fw_path, R_OK) != 0)
    {
        printf("[BT] Firmware not found: %s\n", fw_path);
    }
    else if (bt_load_firmware_direct(uart_dev, fw_path) == 0)
    {
        /* 等待内核创建 hci0 */
        printf("[5/7] Waiting for HCI device...\n");
        for (int i = 0; i < 10; i++)
        {
            usleep(500000);
            if (access("/sys/class/bluetooth/hci0", F_OK) == 0)
            {
                printf("[BT] HCI device detected after %d.%ds\n",
                       (i + 1) / 2, (i + 1) % 2 * 5);
                hci_ready = 1;
                break;
            }
            hci_dev_id = hci_get_route(NULL);
            if (hci_dev_id >= 0)
            {
                hci_ready = 1;
                break;
            }
        }

        if (!hci_ready)
        {
            printf("[BT] Direct method: N_HCI set but hci0 not created\n");
            /* 关闭 fd, 后续方法需要 UART */
            if (hci_uart_fd >= 0)
            {
                close(hci_uart_fd);
                hci_uart_fd = -1;
            }
        }
    }
    else
    {
        printf("[BT] Direct firmware loading failed\n");
    }

    /* ---------- 方法 B: brcm_patchram_plus1 回退 ---------- */
    if (!hci_ready)
    {
        printf("[BT] Trying brcm_patchram_plus1...\n");

        /* 清理并重置 */
        setup_uart(uart_dev, 115200);
        sleep(1);
        bt_power_cycle();

        snprintf(cmd, sizeof(cmd),
                 "cd /system/etc/firmware && "
                 "/usr/bin/brcm_patchram_plus1 "
                 "--patchram %s "
                 "--baudrate 1500000 "
                 "--enable_hci "
                 "--no2bytes "
                 "--tosleep 200000 "
                 "%s > /tmp/bt_patch.log 2>&1 &",
                 fw_path, uart_dev);

        printf("[Debug] Executing: %s\n", cmd);
        system(cmd);

        for (int i = 0; i < 20; i++)
        {
            sleep(1);
            if (access("/sys/class/bluetooth/hci0", F_OK) == 0)
            {
                printf("[BT] HCI device detected (brcm_patchram_plus1) after %ds\n", i + 1);
                hci_ready = 1;
                break;
            }
            if ((i + 1) % 3 == 0)
            {
                if (system("ps | grep -q '[b]rcm_patchram_plus1'") != 0)
                {
                    printf("[BT] brcm_patchram_plus1 exited early\n");
                    system("cat /tmp/bt_patch.log 2>/dev/null");
                    break;
                }
            }
        }
    }

    /* ---------- 方法 C: hciattach 回退 ---------- */
    if (!hci_ready)
    {
        printf("[BT] Trying hciattach...\n");

        execute_cmd("killall -9 brcm_patchram_plus1 2>/dev/null");
        sleep(1);
        setup_uart(uart_dev, 115200);
        sleep(1);
        bt_power_cycle();

        /* 创建 hciattach bcm43xx 需要的固件符号链接 */
        system("mkdir -p /lib/firmware/brcm 2>/dev/null");
        if (access("/lib/firmware/brcm/BCM43430A1.hcd", F_OK) != 0)
            symlink(fw_path, "/lib/firmware/brcm/BCM43430A1.hcd");
        if (access("/lib/firmware/brcm/BCM.hcd", F_OK) != 0)
            symlink(fw_path, "/lib/firmware/brcm/BCM.hcd");

        snprintf(cmd, sizeof(cmd),
                 "hciattach -s 115200 %s bcm43xx 1500000 flow "
                 "> /tmp/bt_hciattach.log 2>&1 &",
                 uart_dev);
        printf("[Debug] Executing: %s\n", cmd);
        system(cmd);

        for (int i = 0; i < 15; i++)
        {
            sleep(1);
            if (access("/sys/class/bluetooth/hci0", F_OK) == 0)
            {
                printf("[BT] HCI device detected (hciattach) after %ds\n", i + 1);
                hci_ready = 1;
                break;
            }
            if ((i + 1) % 3 == 0 && system("ps | grep -q '[h]ciattach'") != 0)
            {
                printf("[BT] hciattach exited early\n");
                system("cat /tmp/bt_hciattach.log 2>/dev/null");
                break;
            }
        }
    }

    if (!hci_ready)
    {
        /* 最后检查 */
        printf("[BT] Last attempt: checking hciconfig...\n");
        system("hciconfig -a 2>/dev/null");

        hci_dev_id = hci_get_route(NULL);
        if (hci_dev_id >= 0)
        {
            printf("[BT] HCI device found on final check\n");
            hci_ready = 1;
        }
        else
        {
            printf("[Error] HCI device not created after all attempts\n");
            printf("[Debug] Checking kernel logs...\n");
            system("dmesg | grep -i 'bluetooth\\|hci\\|bcm\\|brcm\\|uart1' | tail -15");
            return -1;
        }
    }

    /* ===== 6. 启用 hci0 ===== */
    printf("[6/7] Bringing up hci0...\n");
    for (int retry = 0; retry < 5; retry++)
    {
        if (execute_cmd("hciconfig hci0 up") == 0)
        {
            printf("[BT] hci0 brought up successfully\n");
            break;
        }
        printf("  Retry %d/5...\n", retry + 1);
        sleep(2);
    }

    /* ===== 7. 验证 HCI 设备 ===== */
    printf("[7/7] Verifying HCI device...\n");
    hci_dev_id = hci_get_route(NULL);
    if (hci_dev_id < 0)
    {
        printf("[Error] No HCI device found after initialization\n");
        return -1;
    }

    hci_sock = hci_open_dev(hci_dev_id);
    if (hci_sock < 0)
    {
        printf("[Error] Failed to open HCI device: %s\n", strerror(errno));
        return -1;
    }

    /* 打印设备信息 */
    system("hciconfig hci0");

    printf("[BT] Initialization successful (hci%d)\n\n", hci_dev_id);
    return 0;
}

int dev_ap6212_bt_ble_scan(int duration_sec, ap6212_ble_scan_cb_t callback)
{
    if (hci_sock < 0)
    {
        printf("[BT] Not initialized, call dev_ap6212_bt_init() first\n");
        return -1;
    }

    int device_count = 0;
    int err;

    printf("\n=== BLE Scan Start (duration: %ds) ===\n\n", duration_sec);

    /* ----- 设置 BLE 扫描参数 ----- */
    /* Type: 0x01 = Active scan (请求 Scan Response)
     * Interval: 0x0010 (10ms)
     * Window:   0x0010 (10ms)
     * Own addr: 0x00 = Public
     * Filter:   0x00 = Accept all */
    err = hci_le_set_scan_parameters(hci_sock,
                                     0x01,          /* scan type: active */
                                     htobs(0x0010), /* interval */
                                     htobs(0x0010), /* window */
                                     0x00,          /* own address type */
                                     0x00,          /* filter policy */
                                     1000);         /* timeout ms */
    if (err < 0)
    {
        printf("[BT] Failed to set scan parameters: %s\n", strerror(errno));
        return -1;
    }

    /* ----- 启用 BLE 扫描 ----- */
    /* Enable: 1, Filter duplicates: 0 (报告所有广播) */
    err = hci_le_set_scan_enable(hci_sock,
                                 0x01, /* enable */
                                 0x00, /* no duplicate filter -> see all */
                                 1000);
    if (err < 0)
    {
        printf("[BT] Failed to enable scan: %s\n", strerror(errno));
        return -1;
    }

    /* ----- 设置 HCI 事件过滤, 只接收 LE Meta Event ----- */
    struct hci_filter old_filter;
    struct hci_filter new_filter;
    socklen_t olen = sizeof(old_filter);

    getsockopt(hci_sock, SOL_HCI, HCI_FILTER, &old_filter, &olen);

    hci_filter_clear(&new_filter);
    hci_filter_set_ptype(HCI_EVENT_PKT, &new_filter);
    hci_filter_set_event(EVT_LE_META_EVENT, &new_filter);
    setsockopt(hci_sock, SOL_HCI, HCI_FILTER, &new_filter, sizeof(new_filter));

    /* ----- 读取广播报告 ----- */
    uint8_t buf[HCI_MAX_EVENT_SIZE];
    time_t start_time = time(NULL);

    printf("%-20s %-8s %-6s %s\n", "Address", "Type", "RSSI", "Name");
    printf("------------------------------------------------------\n");

    while (time(NULL) - start_time < duration_sec)
    {
        struct pollfd pfd;
        pfd.fd = hci_sock;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 1000); /* 1s timeout */
        if (ret <= 0)
            continue;

        ssize_t len = read(hci_sock, buf, sizeof(buf));
        if (len <= 0)
            continue;

        /* 解析 HCI Event:
         * buf[0] = HCI packet type (0x04 = event)
         * buf[1] = event code
         * buf[2] = parameter length
         * buf[3] = subevent code (for LE Meta)
         * buf[4+] = event data */
        uint8_t *ptr = buf + (1 + HCI_EVENT_HDR_SIZE);
        evt_le_meta_event *meta = (evt_le_meta_event *)ptr;

        if (meta->subevent != EVT_LE_ADVERTISING_REPORT)
            continue;

        /* 解析 LE Advertising Report */
        le_advertising_info *info = (le_advertising_info *)(meta->data + 1);

        /* RSSI 位于广播数据之后 */
        int8_t rssi = (int8_t)info->data[info->length];

        /* 构建设备信息 */
        ap6212_ble_device_t dev;
        memset(&dev, 0, sizeof(dev));

        ba2str(&info->bdaddr, dev.address);
        dev.rssi = rssi;
        dev.addr_type = info->bdaddr_type;

        /* 解析设备名称 */
        parse_ble_name(info->data, info->length, dev.name, sizeof(dev.name));

        /* 打印设备信息 */
        printf("%-20s %-8s %4d   %s\n",
               dev.address,
               dev.addr_type == 0 ? "Public" : "Random",
               dev.rssi,
               dev.name[0] ? dev.name : "(unknown)");

        device_count++;

        /* 调用用户回调 */
        if (callback)
        {
            callback(&dev);
        }
    }

    /* ----- 停止扫描 ----- */
    hci_le_set_scan_enable(hci_sock, 0x00, 0x00, 1000);

    /* 恢复原来的过滤器 */
    setsockopt(hci_sock, SOL_HCI, HCI_FILTER, &old_filter, sizeof(old_filter));

    printf("\n=== BLE Scan Complete: %d devices found ===\n\n", device_count);

    return device_count;
}

void dev_ap6212_bt_cleanup(void)
{
    printf("[BT] Cleaning up...\n");

    if (hci_sock >= 0)
    {
        hci_le_set_scan_enable(hci_sock, 0x00, 0x00, 1000);
        hci_close_dev(hci_sock);
        hci_sock = -1;
    }

    hci_dev_id = -1;

    execute_cmd("hciconfig hci0 down 2>/dev/null");

    /* 关闭直接固件加载的 UART fd (释放 N_HCI line discipline) */
    if (hci_uart_fd >= 0)
    {
        close(hci_uart_fd);
        hci_uart_fd = -1;
    }

    printf("[BT] Cleanup done\n");
}