#include "dev_ap6212_ble_gatt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/l2cap.h>

/* ============================================================
 * 常量定义
 * ============================================================ */

/* L2CAP ATT Channel ID */
#define ATT_CID 4

/* ATT Protocol 操作码 */
#define ATT_OP_ERROR 0x01
#define ATT_OP_MTU_REQ 0x02
#define ATT_OP_MTU_RSP 0x03
#define ATT_OP_FIND_INFO_REQ 0x04
#define ATT_OP_FIND_INFO_RSP 0x05
#define ATT_OP_READ_BY_TYPE_REQ 0x08
#define ATT_OP_READ_BY_TYPE_RSP 0x09
#define ATT_OP_READ_REQ 0x0A
#define ATT_OP_READ_RSP 0x0B
#define ATT_OP_READ_BY_GRP_REQ 0x10
#define ATT_OP_READ_BY_GRP_RSP 0x11
#define ATT_OP_WRITE_REQ 0x12
#define ATT_OP_WRITE_RSP 0x13
#define ATT_OP_WRITE_CMD 0x52
#define ATT_OP_HANDLE_NOTIFY 0x1B
#define ATT_OP_HANDLE_IND 0x1D
#define ATT_OP_HANDLE_CNF 0x1E

/* GATT 标准 UUID */
#define GATT_PRIM_SVC_UUID 0x2800
#define GATT_CHARAC_UUID 0x2803
#define GATT_CCCD_UUID 0x2902

/* 兼容性宏 (部分旧版 BlueZ 可能缺少) */
#ifndef BDADDR_LE_PUBLIC
#define BDADDR_LE_PUBLIC 0x01
#endif

#ifndef BDADDR_LE_RANDOM
#define BDADDR_LE_RANDOM 0x02
#endif

#ifndef BT_SECURITY
#define BT_SECURITY 4
struct bt_security
{
    uint8_t level;
    uint8_t key_size;
};
#endif

#ifndef BT_SECURITY_LOW
#define BT_SECURITY_LOW 1
#endif

/* ============================================================
 * Nordic UART Service (NUS) UUID 定义 (小端序)
 *
 * 与 ESP32-S3 端 ble_string_svc.c 中的定义对应:
 *   Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX:      6E400002-B5A3-F393-E0A9-E50E24DCCA9E (central 写入)
 *   TX:      6E400003-B5A3-F393-E0A9-E50E24DCCA9E (central 接收通知)
 * ============================================================ */

static const uint8_t NUS_SVC_UUID128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E};

static const uint8_t NUS_RX_UUID128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E};

static const uint8_t NUS_TX_UUID128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E};

/* ============================================================
 * 全局状态
 * ============================================================ */

static ble_conn_t g_conn;
static ble_notify_cb_t g_notify_cb = NULL;

/**
 * @brief 比较 128-bit UUID
 */
static int uuid128_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 16) == 0;
}

/* ============================================================
 * ATT 低层收发
 * ============================================================ */

static int att_send(const uint8_t *data, int len)
{
    if (g_conn.l2cap_sock < 0)
        return -1;

    int ret = write(g_conn.l2cap_sock, data, len);
    if (ret < 0)
    {
        printf("[BLE] ATT send error: %s\n", strerror(errno));
        return -1;
    }
    return ret;
}

static int att_recv(uint8_t *data, int max_len, int timeout_ms)
{
    struct pollfd pfd = {
        .fd = g_conn.l2cap_sock,
        .events = POLLIN};

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return ret; /* 0=超时, -1=错误 */

    ret = read(g_conn.l2cap_sock, data, max_len);
    if (ret < 0)
    {
        printf("[BLE] ATT recv error: %s\n", strerror(errno));
        return -1;
    }
    return ret;
}

/**
 * @brief 接收 ATT 响应, 自动处理中间穿插的通知/指示
 */
static int att_recv_response(uint8_t *data, int max_len, int timeout_ms)
{
    struct timeval start, now;
    gettimeofday(&start, NULL);

    while (1)
    {
        gettimeofday(&now, NULL);
        int elapsed = (now.tv_sec - start.tv_sec) * 1000 +
                      (now.tv_usec - start.tv_usec) / 1000;
        int remaining = timeout_ms - elapsed;
        if (remaining <= 0)
            return 0;

        int len = att_recv(data, max_len, remaining);
        if (len <= 0)
            return len;

        /* 如果收到通知, 处理后继续等待真正的响应 */
        if (data[0] == ATT_OP_HANDLE_NOTIFY && len >= 3)
        {
            if (g_notify_cb)
            {
                uint16_t handle = data[1] | (data[2] << 8);
                g_notify_cb(handle, &data[3], len - 3);
            }
            continue;
        }

        if (data[0] == ATT_OP_HANDLE_IND && len >= 3)
        {
            /* 发送确认 */
            uint8_t cnf = ATT_OP_HANDLE_CNF;
            att_send(&cnf, 1);

            if (g_notify_cb)
            {
                uint16_t handle = data[1] | (data[2] << 8);
                g_notify_cb(handle, &data[3], len - 3);
            }
            continue;
        }

        return len; /* 真正的 ATT 响应 */
    }
}

/* ============================================================
 * ATT 协议操作
 * ============================================================ */

/**
 * @brief 交换 MTU
 */
static int att_exchange_mtu(uint16_t our_mtu)
{
    uint8_t req[3];
    req[0] = ATT_OP_MTU_REQ;
    req[1] = our_mtu & 0xFF;
    req[2] = (our_mtu >> 8) & 0xFF;

    if (att_send(req, 3) < 0)
        return -1;

    uint8_t rsp[3];
    int len = att_recv_response(rsp, sizeof(rsp), 5000);
    if (len < 3 || rsp[0] != ATT_OP_MTU_RSP)
    {
        printf("[BLE] MTU exchange failed\n");
        /* 使用默认 MTU */
        g_conn.mtu = 23;
        return -1;
    }

    uint16_t server_mtu = rsp[1] | (rsp[2] << 8);
    g_conn.mtu = (our_mtu < server_mtu) ? our_mtu : server_mtu;
    if (g_conn.mtu < 23)
        g_conn.mtu = 23;

    printf("[BLE] MTU negotiated: %d (ours=%d, server=%d)\n",
           g_conn.mtu, our_mtu, server_mtu);
    return 0;
}

/**
 * @brief 发现所有 Primary Services
 */
static int att_discover_primary_services(void)
{
    g_conn.service_count = 0;
    uint16_t start_handle = 0x0001;

    while (start_handle < 0xFFFF && g_conn.service_count < BLE_MAX_SERVICES)
    {
        uint8_t req[7];
        req[0] = ATT_OP_READ_BY_GRP_REQ;
        req[1] = start_handle & 0xFF;
        req[2] = (start_handle >> 8) & 0xFF;
        req[3] = 0xFF; /* end handle low */
        req[4] = 0xFF; /* end handle high */
        req[5] = GATT_PRIM_SVC_UUID & 0xFF;
        req[6] = (GATT_PRIM_SVC_UUID >> 8) & 0xFF;

        if (att_send(req, 7) < 0)
            return -1;

        uint8_t rsp[256];
        int len = att_recv_response(rsp, sizeof(rsp), 5000);
        if (len < 2)
            return -1;

        if (rsp[0] == ATT_OP_ERROR)
        {
            /* 0x0A = Attribute Not Found, 正常结束 */
            break;
        }

        if (rsp[0] != ATT_OP_READ_BY_GRP_RSP)
        {
            printf("[BLE] Unexpected service discovery response: 0x%02X\n", rsp[0]);
            return -1;
        }

        uint8_t attr_data_len = rsp[1];
        int offset = 2;

        while (offset + attr_data_len <= len &&
               g_conn.service_count < BLE_MAX_SERVICES)
        {
            ble_service_t *svc = &g_conn.services[g_conn.service_count];

            svc->start_handle = rsp[offset] | (rsp[offset + 1] << 8);
            svc->end_handle = rsp[offset + 2] | (rsp[offset + 3] << 8);

            if (attr_data_len == 6)
            {
                /* 16-bit UUID */
                svc->uuid.type = 0;
                svc->uuid.uuid16 = rsp[offset + 4] | (rsp[offset + 5] << 8);
            }
            else if (attr_data_len == 20)
            {
                /* 128-bit UUID */
                svc->uuid.type = 1;
                memcpy(svc->uuid.uuid128, &rsp[offset + 4], 16);
            }

            g_conn.service_count++;
            start_handle = svc->end_handle + 1;
            offset += attr_data_len;
        }
    }

    return g_conn.service_count;
}

/**
 * @brief 发现指定句柄范围内的 Characteristics
 */
static int att_discover_characteristics(uint16_t start, uint16_t end)
{
    while (start <= end && g_conn.char_count < BLE_MAX_CHARS)
    {
        uint8_t req[7];
        req[0] = ATT_OP_READ_BY_TYPE_REQ;
        req[1] = start & 0xFF;
        req[2] = (start >> 8) & 0xFF;
        req[3] = end & 0xFF;
        req[4] = (end >> 8) & 0xFF;
        req[5] = GATT_CHARAC_UUID & 0xFF;
        req[6] = (GATT_CHARAC_UUID >> 8) & 0xFF;

        if (att_send(req, 7) < 0)
            return -1;

        uint8_t rsp[256];
        int len = att_recv_response(rsp, sizeof(rsp), 5000);
        if (len < 2)
            return -1;

        if (rsp[0] == ATT_OP_ERROR)
            break; /* 没有更多特征 */

        if (rsp[0] != ATT_OP_READ_BY_TYPE_RSP)
        {
            printf("[BLE] Unexpected char discovery response: 0x%02X\n", rsp[0]);
            return -1;
        }

        uint8_t attr_data_len = rsp[1];
        int offset = 2;

        while (offset + attr_data_len <= len &&
               g_conn.char_count < BLE_MAX_CHARS)
        {
            ble_characteristic_t *chr = &g_conn.chars[g_conn.char_count];

            chr->decl_handle = rsp[offset] | (rsp[offset + 1] << 8);
            chr->properties = rsp[offset + 2];
            chr->value_handle = rsp[offset + 3] | (rsp[offset + 4] << 8);

            if (attr_data_len == 7)
            {
                /* 16-bit UUID */
                chr->uuid.type = 0;
                chr->uuid.uuid16 = rsp[offset + 5] | (rsp[offset + 6] << 8);
            }
            else if (attr_data_len == 21)
            {
                /* 128-bit UUID */
                chr->uuid.type = 1;
                memcpy(chr->uuid.uuid128, &rsp[offset + 5], 16);
            }

            g_conn.char_count++;
            offset += attr_data_len;
        }

        /* 继续从最后发现的特征之后搜索 */
        if (g_conn.char_count > 0)
        {
            start = g_conn.chars[g_conn.char_count - 1].value_handle + 1;
        }
        else
        {
            break;
        }
    }

    return 0;
}

/**
 * @brief 在句柄范围内查找 CCCD (Client Characteristic Configuration Descriptor)
 * @return CCCD 句柄, 0 表示未找到
 */
static uint16_t att_find_cccd(uint16_t start, uint16_t end)
{
    if (start > end)
        return 0;

    uint8_t req[5];
    req[0] = ATT_OP_FIND_INFO_REQ;
    req[1] = start & 0xFF;
    req[2] = (start >> 8) & 0xFF;
    req[3] = end & 0xFF;
    req[4] = (end >> 8) & 0xFF;

    if (att_send(req, 5) < 0)
        return 0;

    uint8_t rsp[256];
    int len = att_recv_response(rsp, sizeof(rsp), 5000);
    if (len < 2 || rsp[0] != ATT_OP_FIND_INFO_RSP)
        return 0;

    uint8_t format = rsp[1]; /* 1 = 16-bit UUID, 2 = 128-bit UUID */
    int offset = 2;

    if (format == 1)
    {
        /* Handle-UUID16 对 (每个 4 字节) */
        while (offset + 4 <= len)
        {
            uint16_t handle = rsp[offset] | (rsp[offset + 1] << 8);
            uint16_t uuid = rsp[offset + 2] | (rsp[offset + 3] << 8);

            if (uuid == GATT_CCCD_UUID)
            {
                return handle;
            }
            offset += 4;
        }
    }

    return 0;
}

/**
 * @brief 启用通知 (写入 CCCD = 0x0001)
 */
static int att_enable_notify(uint16_t cccd_handle)
{
    uint8_t req[5];
    req[0] = ATT_OP_WRITE_REQ;
    req[1] = cccd_handle & 0xFF;
    req[2] = (cccd_handle >> 8) & 0xFF;
    req[3] = 0x01; /* 启用通知 */
    req[4] = 0x00;

    if (att_send(req, 5) < 0)
        return -1;

    uint8_t rsp[5];
    int len = att_recv_response(rsp, sizeof(rsp), 5000);
    if (len < 1 || rsp[0] != ATT_OP_WRITE_RSP)
    {
        printf("[BLE] Failed to enable notifications (0x%02X)\n",
               len > 0 ? rsp[0] : 0xFF);
        return -1;
    }

    return 0;
}

/* ============================================================
 * BLE 扫描: 按设备名称查找
 * ============================================================ */

static int scan_for_device(const char *target_name, int timeout_sec,
                           bdaddr_t *out_addr, uint8_t *out_addr_type)
{
    int dev_id = hci_get_route(NULL);
    if (dev_id < 0)
    {
        printf("[BLE] No HCI device available\n");
        return -1;
    }

    int sock = hci_open_dev(dev_id);
    if (sock < 0)
    {
        printf("[BLE] Failed to open HCI device: %s\n", strerror(errno));
        return -1;
    }

    /* 设置 LE 扫描参数 */
    int err = hci_le_set_scan_parameters(sock,
                                         0x01,          /* Active scan */
                                         htobs(0x0010), /* interval */
                                         htobs(0x0010), /* window */
                                         0x00,          /* own addr type */
                                         0x00,          /* filter policy */
                                         1000);
    if (err < 0)
    {
        printf("[BLE] Failed to set scan params: %s\n", strerror(errno));
        hci_close_dev(sock);
        return -1;
    }

    /* 启用扫描 */
    err = hci_le_set_scan_enable(sock, 0x01, 0x00, 1000);
    if (err < 0)
    {
        printf("[BLE] Failed to enable scan: %s\n", strerror(errno));
        hci_close_dev(sock);
        return -1;
    }

    /* 设置 HCI 事件过滤 */
    struct hci_filter old_flt, new_flt;
    socklen_t olen = sizeof(old_flt);
    getsockopt(sock, SOL_HCI, HCI_FILTER, &old_flt, &olen);

    hci_filter_clear(&new_flt);
    hci_filter_set_ptype(HCI_EVENT_PKT, &new_flt);
    hci_filter_set_event(EVT_LE_META_EVENT, &new_flt);
    setsockopt(sock, SOL_HCI, HCI_FILTER, &new_flt, sizeof(new_flt));

    /* 扫描循环 */
    uint8_t buf[HCI_MAX_EVENT_SIZE];
    time_t start = time(NULL);
    int found = 0;

    printf("[BLE] Scanning for device '%s' (timeout: %ds)...\n",
           target_name, timeout_sec);

    while (time(NULL) - start < timeout_sec && !found)
    {
        struct pollfd pfd = {.fd = sock, .events = POLLIN};
        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0)
            continue;

        ssize_t len = read(sock, buf, sizeof(buf));
        if (len <= 0)
            continue;

        uint8_t *ptr = buf + (1 + HCI_EVENT_HDR_SIZE);
        evt_le_meta_event *meta = (evt_le_meta_event *)ptr;

        if (meta->subevent != EVT_LE_ADVERTISING_REPORT)
            continue;

        /* 可能包含多个广播报告 */
        uint8_t num_reports = meta->data[0];
        uint8_t *report_ptr = meta->data + 1;

        for (int r = 0; r < num_reports && !found; r++)
        {
            le_advertising_info *info = (le_advertising_info *)report_ptr;

            /* 解析设备名称 */
            char name[64] = {0};
            size_t offset = 0;
            while (offset < info->length)
            {
                uint8_t field_len = info->data[offset];
                if (field_len == 0 || offset + field_len >= info->length)
                    break;

                uint8_t field_type = info->data[offset + 1];
                if (field_type == 0x09 || field_type == 0x08)
                {
                    size_t copy_len = field_len - 1;
                    if (copy_len >= sizeof(name))
                        copy_len = sizeof(name) - 1;
                    memcpy(name, &info->data[offset + 2], copy_len);
                    name[copy_len] = '\0';
                    break;
                }
                offset += field_len + 1;
            }

            if (name[0] && strcmp(name, target_name) == 0)
            {
                bacpy(out_addr, &info->bdaddr);
                *out_addr_type = info->bdaddr_type;

                char addr_str[18];
                ba2str(&info->bdaddr, addr_str);
                printf("[BLE] Found '%s' at %s (type: %s)\n",
                       target_name, addr_str,
                       info->bdaddr_type == 0 ? "Public" : "Random");
                found = 1;
            }

            /* 移动到下一个广播报告 */
            report_ptr += sizeof(le_advertising_info) + info->length + 1;
        }
    }

    /* 停止扫描 */
    hci_le_set_scan_enable(sock, 0x00, 0x00, 1000);
    setsockopt(sock, SOL_HCI, HCI_FILTER, &old_flt, sizeof(old_flt));
    hci_close_dev(sock);

    if (!found)
    {
        printf("[BLE] Device '%s' not found within %d seconds\n",
               target_name, timeout_sec);
    }

    return found ? 0 : -1;
}

/* ============================================================
 * L2CAP BLE 连接 (含安全配对)
 * ============================================================ */

#ifndef BT_SECURITY_MEDIUM
#define BT_SECURITY_MEDIUM 2
#endif

static int l2cap_ble_connect(const bdaddr_t *dst, uint8_t dst_type)
{
    char addr_str[18];
    ba2str(dst, addr_str);

    /*
     * 直接通过 L2CAP 套接字连接 BLE 设备.
     * 内核会自动处理:
     *   1) 建立 LE 连接
     *   2) SMP 配对 (Just Works, 由 BT_SECURITY_MEDIUM 触发)
     *   3) 建立加密链路
     *   4) 完成 L2CAP ATT 连接
     *
     * ESP32 HID 设备要求加密, 所以必须设置 BT_SECURITY_MEDIUM.
     */

    int max_retries = 3;
    int security_levels[] = {BT_SECURITY_MEDIUM, BT_SECURITY_LOW, BT_SECURITY_MEDIUM};
    int sock = -1;

    for (int retry = 0; retry < max_retries; retry++)
    {
        if (retry > 0)
        {
            printf("[BLE] Retry %d/%d (wait 3s)...\n", retry + 1, max_retries);
            sleep(3);
        }

        sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        if (sock < 0)
        {
            printf("[BLE] Failed to create L2CAP socket: %s\n", strerror(errno));
            continue;
        }

        /* 绑定本地地址 */
        struct sockaddr_l2 local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.l2_family = AF_BLUETOOTH;
        local_addr.l2_cid = htobs(ATT_CID);
        local_addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
        bacpy(&local_addr.l2_bdaddr, BDADDR_ANY);

        if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0)
        {
            printf("[BLE] L2CAP bind failed: %s\n", strerror(errno));
            close(sock);
            sock = -1;
            continue;
        }

        /*
         * 设置安全级别:
         * BT_SECURITY_MEDIUM (2) = Just Works 配对 + 加密
         * 这满足 ESP32 HID 服务的安全要求.
         * 内核 SMP 模块会自动完成 Just Works 配对.
         */
        struct bt_security sec;
        memset(&sec, 0, sizeof(sec));
        sec.level = security_levels[retry];
        printf("[BLE] Setting security level: %d\n", sec.level);
        if (setsockopt(sock, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) < 0)
        {
            printf("[BLE] Warning: set security failed: %s\n", strerror(errno));
        }

        /* 连接远端设备 */
        struct sockaddr_l2 remote_addr;
        memset(&remote_addr, 0, sizeof(remote_addr));
        remote_addr.l2_family = AF_BLUETOOTH;
        remote_addr.l2_cid = htobs(ATT_CID);
        bacpy(&remote_addr.l2_bdaddr, dst);
        remote_addr.l2_bdaddr_type = (dst_type == 0) ? BDADDR_LE_PUBLIC : BDADDR_LE_RANDOM;

        printf("[BLE] Connecting to %s (type: %s)...\n",
               addr_str,
               dst_type == 0 ? "LE_PUBLIC" : "LE_RANDOM");

        if (connect(sock, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0)
        {
            printf("[BLE] L2CAP connect attempt %d failed: %s (errno=%d)\n",
                   retry + 1, strerror(errno), errno);
            close(sock);
            sock = -1;
            continue;
        }

        printf("[BLE] L2CAP connected successfully (security level %d)\n",
               security_levels[retry]);
        break;
    }

    if (sock < 0)
    {
        printf("[BLE] All connection attempts failed\n");
    }

    return sock;
}

/* ============================================================
 * UUID 辅助函数
 * ============================================================ */

static void uuid_to_str(const ble_uuid_t *uuid, char *str, size_t len)
{
    if (uuid->type == 0)
    {
        snprintf(str, len, "0x%04X", uuid->uuid16);
    }
    else
    {
        const uint8_t *u = uuid->uuid128;
        snprintf(str, len,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 u[15], u[14], u[13], u[12],
                 u[11], u[10], u[9], u[8],
                 u[7], u[6], u[5], u[4],
                 u[3], u[2], u[1], u[0]);
    }
}

/* ============================================================
 * 公有 API 实现
 * ============================================================ */

int dev_ap6212_ble_connect(const char *device_name, int scan_timeout_sec)
{
    printf("\n=== BLE Connect to '%s' ===\n\n", device_name);

    memset(&g_conn, 0, sizeof(g_conn));
    g_conn.l2cap_sock = -1;
    g_conn.mtu = 23; /* 默认 ATT MTU */

    /* ===== 1. 扫描目标设备 ===== */
    printf("[1/5] Scanning for BLE device...\n");
    bdaddr_t addr;
    uint8_t addr_type;

    if (scan_for_device(device_name, scan_timeout_sec, &addr, &addr_type) < 0)
    {
        printf("[Error] Device '%s' not found\n", device_name);
        return -1;
    }

    ba2str(&addr, g_conn.device_addr);
    g_conn.addr_type = addr_type;
    strncpy(g_conn.device_name, device_name, sizeof(g_conn.device_name) - 1);

    /* 扫描结束后等待, 确保 HCI 扫描状态已完全清除 */
    printf("[BLE] Scan stopped, waiting 2s before connecting...\n");
    sleep(2);

    /* ===== 2. 建立 BLE 连接 (HCI LE + L2CAP ATT) ===== */
    printf("[2/5] Connecting via BLE...\n");
    g_conn.l2cap_sock = l2cap_ble_connect(&addr, addr_type);
    if (g_conn.l2cap_sock < 0)
    {
        return -1;
    }

    /* ===== 3. 交换 MTU ===== */
    printf("[3/5] Exchanging MTU...\n");
    att_exchange_mtu(517); /* BLE 5.x 最大支持 517 */

    /* ===== 4. 发现服务和特征 ===== */
    printf("[4/5] Discovering GATT services and characteristics...\n");

    int svc_count = att_discover_primary_services();
    if (svc_count < 0)
    {
        printf("[Error] Service discovery failed\n");
        close(g_conn.l2cap_sock);
        g_conn.l2cap_sock = -1;
        return -1;
    }

    printf("[BLE] Found %d service(s):\n", g_conn.service_count);
    for (int i = 0; i < g_conn.service_count; i++)
    {
        char uuid_str[48];
        uuid_to_str(&g_conn.services[i].uuid, uuid_str, sizeof(uuid_str));
        printf("  Service %d: handles [0x%04X - 0x%04X] UUID: %s\n",
               i + 1,
               g_conn.services[i].start_handle,
               g_conn.services[i].end_handle,
               uuid_str);
    }

    /* 发现每个服务中的特征 */
    g_conn.char_count = 0;
    for (int i = 0; i < g_conn.service_count; i++)
    {
        att_discover_characteristics(
            g_conn.services[i].start_handle,
            g_conn.services[i].end_handle);
    }

    printf("[BLE] Found %d characteristic(s):\n", g_conn.char_count);
    for (int i = 0; i < g_conn.char_count; i++)
    {
        char uuid_str[48];
        uuid_to_str(&g_conn.chars[i].uuid, uuid_str, sizeof(uuid_str));
        printf("  Char %d: decl=0x%04X val=0x%04X props=0x%02X UUID=%s [",
               i + 1,
               g_conn.chars[i].decl_handle,
               g_conn.chars[i].value_handle,
               g_conn.chars[i].properties,
               uuid_str);

        if (g_conn.chars[i].properties & BLE_PROP_READ)
            printf("R");
        if (g_conn.chars[i].properties & BLE_PROP_WRITE)
            printf("W");
        if (g_conn.chars[i].properties & BLE_PROP_WRITE_NO_RSP)
            printf("w");
        if (g_conn.chars[i].properties & BLE_PROP_NOTIFY)
            printf("N");
        if (g_conn.chars[i].properties & BLE_PROP_INDICATE)
            printf("I");
        printf("]\n");
    }

    /* ===== 5. 定位 NUS 服务的 TX/RX 特征并启用通知 ===== */
    printf("[5/5] Searching for NUS (Nordic UART Service) characteristics...\n");

    g_conn.rx_handle = 0;
    g_conn.tx_handle = 0;
    g_conn.tx_cccd_handle = 0;

    /* 先查找 NUS 服务的句柄范围 */
    uint16_t nus_start = 0, nus_end = 0;
    for (int i = 0; i < g_conn.service_count; i++)
    {
        if (g_conn.services[i].uuid.type == 1 &&
            uuid128_equal(g_conn.services[i].uuid.uuid128, NUS_SVC_UUID128))
        {
            nus_start = g_conn.services[i].start_handle;
            nus_end = g_conn.services[i].end_handle;
            printf("[BLE] Found NUS service: handles [0x%04X - 0x%04X]\n",
                   nus_start, nus_end);
            break;
        }
    }

    if (nus_start && nus_end)
    {
        /* 在 NUS 服务范围内按 UUID 精确匹配 RX 和 TX 特征 */
        for (int i = 0; i < g_conn.char_count; i++)
        {
            ble_characteristic_t *c = &g_conn.chars[i];

            /* 跳过不在 NUS 服务范围内的特征 */
            if (c->decl_handle < nus_start || c->decl_handle > nus_end)
                continue;

            if (c->uuid.type != 1)
                continue;

            /* NUS RX: 6E400002... (central 写入 → ESP32 接收) */
            if (!g_conn.rx_handle &&
                uuid128_equal(c->uuid.uuid128, NUS_RX_UUID128))
            {
                g_conn.rx_handle = c->value_handle;
                printf("[BLE] NUS RX characteristic found: 0x%04X\n",
                       g_conn.rx_handle);
            }

            /* NUS TX: 6E400003... (ESP32 通知 → central 接收) */
            if (!g_conn.tx_handle &&
                uuid128_equal(c->uuid.uuid128, NUS_TX_UUID128))
            {
                g_conn.tx_handle = c->value_handle;
                printf("[BLE] NUS TX characteristic found: 0x%04X\n",
                       g_conn.tx_handle);

                /* 查找该特征的 CCCD */
                uint16_t cccd_start = c->value_handle + 1;
                uint16_t cccd_end = nus_end;

                if (i + 1 < g_conn.char_count &&
                    g_conn.chars[i + 1].decl_handle <= nus_end)
                {
                    cccd_end = g_conn.chars[i + 1].decl_handle - 1;
                }

                if (cccd_start <= cccd_end)
                {
                    g_conn.tx_cccd_handle = att_find_cccd(cccd_start, cccd_end);
                }
            }
        }
    }
    else
    {
        printf("[Warning] NUS service not found, falling back to generic detection\n");

        /* 回退: 通用自动检测 (第一个可写 / 可通知特征) */
        for (int i = 0; i < g_conn.char_count; i++)
        {
            ble_characteristic_t *c = &g_conn.chars[i];

            if (!g_conn.rx_handle &&
                (c->properties & (BLE_PROP_WRITE | BLE_PROP_WRITE_NO_RSP)))
            {
                g_conn.rx_handle = c->value_handle;
            }

            if (!g_conn.tx_handle &&
                (c->properties & (BLE_PROP_NOTIFY | BLE_PROP_INDICATE)))
            {
                g_conn.tx_handle = c->value_handle;

                uint16_t cccd_start = c->value_handle + 1;
                uint16_t cccd_end = 0xFFFF;

                if (i + 1 < g_conn.char_count)
                {
                    cccd_end = g_conn.chars[i + 1].decl_handle - 1;
                }
                else
                {
                    for (int s = 0; s < g_conn.service_count; s++)
                    {
                        if (c->decl_handle >= g_conn.services[s].start_handle &&
                            c->decl_handle <= g_conn.services[s].end_handle)
                        {
                            cccd_end = g_conn.services[s].end_handle;
                            break;
                        }
                    }
                }

                if (cccd_start <= cccd_end)
                {
                    g_conn.tx_cccd_handle = att_find_cccd(cccd_start, cccd_end);
                }
            }
        }
    }

    if (g_conn.rx_handle)
    {
        printf("[BLE] RX handle (write to remote): 0x%04X\n", g_conn.rx_handle);
    }
    else
    {
        printf("[Warning] No writable characteristic found\n");
    }

    if (g_conn.tx_handle)
    {
        printf("[BLE] TX handle (notify from remote): 0x%04X\n", g_conn.tx_handle);

        if (g_conn.tx_cccd_handle)
        {
            printf("[BLE] Enabling notifications (CCCD: 0x%04X)...\n",
                   g_conn.tx_cccd_handle);
            if (att_enable_notify(g_conn.tx_cccd_handle) == 0)
            {
                printf("[BLE] Notifications enabled successfully\n");
            }
            else
            {
                printf("[Warning] Failed to enable notifications\n");
            }
        }
        else
        {
            printf("[Warning] CCCD not found for TX characteristic\n");
        }
    }
    else
    {
        printf("[Warning] No notifiable characteristic found\n");
    }

    g_conn.connected = 1;
    printf("\n=== BLE Connected to '%s' (%s) ===\n",
           g_conn.device_name, g_conn.device_addr);
    printf("    MTU: %d, Services: %d, Characteristics: %d\n\n",
           g_conn.mtu, g_conn.service_count, g_conn.char_count);

    return 0;
}

int dev_ap6212_ble_disconnect(void)
{
    if (!g_conn.connected)
        return 0;

    printf("[BLE] Disconnecting from '%s'...\n", g_conn.device_name);

    if (g_conn.l2cap_sock >= 0)
    {
        close(g_conn.l2cap_sock);
        g_conn.l2cap_sock = -1;
    }

    g_conn.connected = 0;
    printf("[BLE] Disconnected\n");

    return 0;
}

int dev_ap6212_ble_send(const uint8_t *data, int len)
{
    if (!g_conn.connected)
    {
        printf("[BLE] Not connected\n");
        return -1;
    }

    if (g_conn.rx_handle == 0)
    {
        printf("[BLE] No writable characteristic (rx_handle=0)\n");
        return -1;
    }

    return dev_ap6212_ble_write(g_conn.rx_handle, data, len);
}

int dev_ap6212_ble_receive(uint8_t *data, int max_len, int timeout_ms)
{
    if (!g_conn.connected || g_conn.l2cap_sock < 0)
        return -1;

    uint8_t buf[512];
    int len = att_recv(buf, sizeof(buf), timeout_ms);

    if (len <= 0)
        return len;

    /* 处理通知 */
    if (buf[0] == ATT_OP_HANDLE_NOTIFY && len >= 3)
    {
        uint16_t handle = buf[1] | (buf[2] << 8);
        int data_len = len - 3;

        if (data_len > max_len)
            data_len = max_len;
        memcpy(data, &buf[3], data_len);

        if (g_notify_cb)
        {
            g_notify_cb(handle, &buf[3], len - 3);
        }

        return data_len;
    }

    /* 处理指示 */
    if (buf[0] == ATT_OP_HANDLE_IND && len >= 3)
    {
        /* 发送确认 */
        uint8_t cnf = ATT_OP_HANDLE_CNF;
        att_send(&cnf, 1);

        uint16_t handle = buf[1] | (buf[2] << 8);
        int data_len = len - 3;

        if (data_len > max_len)
            data_len = max_len;
        memcpy(data, &buf[3], data_len);

        if (g_notify_cb)
        {
            g_notify_cb(handle, &buf[3], len - 3);
        }

        return data_len;
    }

    return 0; /* 非通知/指示数据 */
}

void dev_ap6212_ble_set_notify_callback(ble_notify_cb_t cb)
{
    g_notify_cb = cb;
}

const ble_conn_t *dev_ap6212_ble_get_state(void)
{
    return &g_conn;
}

int dev_ap6212_ble_is_connected(void)
{
    return g_conn.connected;
}

int dev_ap6212_ble_write(uint16_t handle, const uint8_t *data, int len)
{
    if (g_conn.l2cap_sock < 0)
        return -1;

    /* 检查长度是否超过 MTU (ATT header = 3 bytes) */
    int max_payload = g_conn.mtu - 3;
    if (len > max_payload)
    {
        printf("[BLE] Data too long (%d > %d), truncating\n", len, max_payload);
        len = max_payload;
    }

    int pdu_len = 3 + len;
    uint8_t *pdu = (uint8_t *)malloc(pdu_len);
    if (!pdu)
        return -1;

    /* 使用 Write Command (无需等待响应, 效率更高) */
    pdu[0] = ATT_OP_WRITE_CMD;
    pdu[1] = handle & 0xFF;
    pdu[2] = (handle >> 8) & 0xFF;
    memcpy(&pdu[3], data, len);

    int ret = att_send(pdu, pdu_len);
    free(pdu);

    return (ret > 0) ? 0 : -1;
}

int dev_ap6212_ble_read(uint16_t handle, uint8_t *data, int max_len)
{
    if (g_conn.l2cap_sock < 0)
        return -1;

    uint8_t req[3];
    req[0] = ATT_OP_READ_REQ;
    req[1] = handle & 0xFF;
    req[2] = (handle >> 8) & 0xFF;

    if (att_send(req, 3) < 0)
        return -1;

    uint8_t rsp[512];
    int len = att_recv_response(rsp, sizeof(rsp), 5000);
    if (len < 1)
        return -1;

    if (rsp[0] == ATT_OP_READ_RSP)
    {
        int data_len = len - 1;
        if (data_len > max_len)
            data_len = max_len;
        memcpy(data, &rsp[1], data_len);
        return data_len;
    }

    if (rsp[0] == ATT_OP_ERROR && len >= 5)
    {
        printf("[BLE] Read error: handle=0x%04X code=0x%02X\n",
               rsp[2] | (rsp[3] << 8), rsp[4]);
    }

    return -1;
}
