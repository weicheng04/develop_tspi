#include "dev_ap6212_wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ap6212_wifi_status_t wifi_status = AP6212_WIFI_STATUS_DISCONNECTED;
static ap6212_wifi_config_t wifi_config;

/**
 * @brief 执行系统命令的辅助函数，执行给定的命令并检查返回值。
 *
 * @param command 要执行的系统命令字符串。
 *
 * @return 返回0表示命令执行成功，返回非0表示命令执行失败。
 */
static int execute_command(const char *command)
{
    int ret = system(command);
    if (ret != 0)
    {
        printf("[Error] Command Failed: %s\n", command);
    }
    return ret;
}

/**
 * @brief 执行系统命令并获取输出的辅助函数，执行给定的命令并将输出存储在提供的缓冲区中。
 *
 * @param command 要执行的系统命令字符串。
 * @param output 用于存储命令输出的缓冲区。
 * @param output_size 缓冲区的大小，以字节为单位。
 *
 * @return 返回0表示命令执行成功并成功获取输出，返回-1表示命令执行失败或获取输出失败。
 */
static int execute_command_with_output(const char *command, char *output, size_t output_size)
{
    FILE *fp = popen(command, "r");
    if (fp == NULL)
    {
        printf("[Error] Failed To Run Command: %s\n", command);
        return -1;
    }

    if (fgets(output, output_size, fp) == NULL)
    {
        printf("[Error] Failed To Read Command Output: %s\n", command);
        pclose(fp);
        return -1;
    }

    // 去除输出中的换行符
    output[strcspn(output, "\n")] = '\0';

    pclose(fp);
    return 0;
}

/**
 * @brief 检查指定的网络接口是否存在，使用系统命令检查接口状态。
 *
 * @param interface 要检查的网络接口名称（例如 "wlan0"）。
 *
 * @return 返回1表示接口存在，返回0表示接口不存在。
 */
static int check_interface_exists(const char *interface)
{
    char command[128];
    snprintf(command, sizeof(command), "ip link show %s > /dev/null 2>&1", interface);
    return (system(command) == 0);
}

/**
 * @brief 初始化WiFi连接，设置默认配置并准备连接。
 *
 * @return 返回0表示初始化成功，返回-1表示初始化失败。
 */
int dev_ap6212_wifi_init(void)
{
    wifi_status = AP6212_WIFI_STATUS_DISCONNECTED;
    memset(&wifi_config, 0, sizeof(ap6212_wifi_config_t));

    // 设置默认网卡接口
    strncpy(wifi_config.interface, "wlan0", sizeof(wifi_config.interface) - 1);
    wifi_config.max_retries = 15;
    wifi_config.retry_interval_sec = 2;

    return 0;
}

/**
 * @brief 连接到WiFi网络，使用提供的配置参数进行连接。
 *
 * @param config 指向ap6212_wifi_config_t结构体的指针，包含连接所需的配置信息。
 *
 * @return 返回0表示连接成功，返回-1表示连接失败。
 */
int dev_ap6212_wifi_connect(const ap6212_wifi_config_t *config)
{
    if (config == NULL)
    {
        printf("[WiFi] Invalid Configuration\n");
        return -1;
    }

    // 复制配置
    memcpy(&wifi_config, config, sizeof(ap6212_wifi_config_t));

    char cmd[512];
    int retry;

    printf("=== WiFi Connecting ===\n");
    printf("SSID: %s\n", config->ssid);
    printf("Interface: %s\n\n", config->interface);

    // 检查接口是否存在
    if (!check_interface_exists(config->interface))
    {
        printf("[Error] WiFi Interface %s Not Found\n", config->interface);
        wifi_status = AP6212_WIFI_STATUS_ERROR;
        return -1;
    }

    wifi_status = AP6212_WIFI_STATUS_CONNECTING;

    // 清理已存在的进程
    printf("[1/5] Clear Existing Connections...\n");
    execute_command("killall wpa_supplicant 2>/dev/null");
    execute_command("killall udhcpc 2>/dev/null");
    sleep(1);

    // 启动网卡接口
    printf("[2/5] Enable WiFi Interface...\n");
    snprintf(cmd, sizeof(cmd), "ifconfig %s down", config->interface);
    execute_command(cmd);
    sleep(1);

    snprintf(cmd, sizeof(cmd), "ifconfig %s up", config->interface);
    if (execute_command(cmd) != 0)
    {
        printf("[Error] Unable To Start The Interface\n");
        wifi_status = AP6212_WIFI_STATUS_ERROR;
        return -1;
    }
    sleep(2);

    // 创建wpa_supplicant配置文件
    printf("[3/5] Create WiFi Configuration...\n");
    FILE *fp = fopen("/tmp/wpa_mgr.conf", "w");
    if (!fp)
    {
        printf("[Error] Unable To Create Profile\n");
        wifi_status = AP6212_WIFI_STATUS_ERROR;
        return -1;
    }

    fprintf(fp, "ctrl_interface=/var/run/wpa_supplicant\n");
    fprintf(fp, "update_config=1\n");
    fprintf(fp, "ap_scan=1\n\n");
    fprintf(fp, "network={\n");
    fprintf(fp, "    ssid=\"%s\"\n", config->ssid);
    fprintf(fp, "    psk=\"%s\"\n", config->password);
    fprintf(fp, "    key_mgmt=WPA-PSK\n");
    fprintf(fp, "    proto=RSN WPA\n");
    fprintf(fp, "    pairwise=CCMP TKIP\n");
    fprintf(fp, "    scan_ssid=1\n");
    fprintf(fp, "}\n");
    fclose(fp);

    // 启动wpa_supplicant
    printf("[4/5] Connect To WiFi...\n");
    snprintf(cmd, sizeof(cmd),
             "wpa_supplicant -B -i %s -c /tmp/wpa_mgr.conf -D nl80211",
             config->interface);

    if (execute_command(cmd) != 0)
    {
        printf("[Error] wpa_supplicant Startup Failed\n");
        wifi_status = AP6212_WIFI_STATUS_ERROR;
        return -1;
    }

    // 等待连接
    printf("Waiting For WiFi Connection...\n");
    for (retry = 0; retry < config->max_retries; retry++)
    {
        sleep(config->retry_interval_sec);

        snprintf(cmd, sizeof(cmd),
                 "iw dev %s link | grep -q Connected", config->interface);

        if (execute_command(cmd) == 0)
        {
            printf("[Success] WiFi is Connected\n");
            break;
        }
        printf("  Retry %d/%d...\n", retry + 1, config->max_retries);
    }

    if (retry >= config->max_retries)
    {
        printf("[Error] WiFi Association Timed Out\n");
        wifi_status = AP6212_WIFI_STATUS_ERROR;
        return -1;
    }

    // 获取IP地址
    printf("[5/5] Get Ip Address...\n");
    snprintf(cmd, sizeof(cmd), "udhcpc -i %s -n -q -t 10", config->interface);
    execute_command(cmd);
    sleep(3);

    // 验证IP
    char ip_check[256];
    snprintf(ip_check, sizeof(ip_check),
             "ifconfig %s | grep 'inet addr' | grep -v '169.254'",
             config->interface);

    if (execute_command(ip_check) != 0)
    {
        printf("[Error] Unable To Obtain A Valid Ip\n");
        wifi_status = AP6212_WIFI_STATUS_ERROR;
        return -1;
    }

    wifi_status = AP6212_WIFI_STATUS_CONNECTED;

    // 显示连接信息
    printf("\n=== WiFi Is Connected ===\n");
    snprintf(cmd, sizeof(cmd), "ifconfig %s | grep 'inet addr'", config->interface);
    execute_command(cmd);

    return 0;
}

/**
 * @brief 断开WiFi连接，清理相关资源并关闭网络接口。
 *
 * @return 返回0表示断开成功，返回-1表示断开失败。
 */
int dev_ap6212_wifi_disconnect(void)
{
    printf("[WiFi] Disconnecting...\n");

    execute_command("killall wpa_supplicant 2>/dev/null");
    execute_command("killall udhcpc 2>/dev/null");

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "ifconfig %s down", wifi_config.interface);
    execute_command(cmd);

    wifi_status = AP6212_WIFI_STATUS_DISCONNECTED;
    printf("[WiFi] Disconnected\n");

    return 0;
}

/**
 * @brief 获取当前WiFi连接状态。
 *
 * @return 返回当前的ap6212_wifi_status_t枚举值，表示WiFi连接的状态。
 */
ap6212_wifi_status_t dev_ap6212_wifi_get_status(void)
{
    return wifi_status;
}

/**
 * @brief 获取当前WiFi连接信息，包括IP地址、MAC地址、信号强度等。
 *
 * @param info 指向ap6212_wifi_info_t结构体的指针，用于存储获取到的WiFi连接信息。
 *
 * @return 返回0表示成功获取信息，返回-1表示获取信息失败。
 */
int dev_ap6212_wifi_get_info(ap6212_wifi_info_t *info)
{
    if (info == NULL)
    {
        return -1;
    }

    memset(info, 0, sizeof(ap6212_wifi_info_t));
    info->status = wifi_status;

    if (wifi_status != AP6212_WIFI_STATUS_CONNECTED)
    {
        return -1;
    }

    char cmd[256];
    char output[128];

    // 获取IP地址
    snprintf(cmd, sizeof(cmd),
             "ifconfig %s | grep 'inet addr' | awk '{print $2}' | cut -d: -f2",
             wifi_config.interface);
    if (execute_command_with_output(cmd, output, sizeof(output)) == 0)
    {
        strncpy(info->ip_address, output, sizeof(info->ip_address) - 1);
    }

    // 获取MAC地址
    snprintf(cmd, sizeof(cmd),
             "ifconfig %s | grep 'HWaddr' | awk '{print $5}'",
             wifi_config.interface);
    if (execute_command_with_output(cmd, output, sizeof(output)) == 0)
    {
        strncpy(info->mac_address, output, sizeof(info->mac_address) - 1);
    }

    // 获取信号强度
    snprintf(cmd, sizeof(cmd),
             "iw dev %s link | grep signal | awk '{print $2}'",
             wifi_config.interface);
    if (execute_command_with_output(cmd, output, sizeof(output)) == 0)
    {
        info->signal_strength = atoi(output);
    }

    return 0;
}

/**
 * @brief 检查当前是否已连接到WiFi网络。
 *
 * @return 返回1表示已连接，返回0表示未连接。
 */
int dev_ap6212_wifi_is_connected(void)
{
    return (wifi_status == AP6212_WIFI_STATUS_CONNECTED) ? 1 : 0;
}

/**
 * @brief 清理WiFi连接资源，如果当前已连接则先断开连接。
 */
void dev_ap6212_wifi_cleanup(void)
{
    if (wifi_status == AP6212_WIFI_STATUS_CONNECTED)
    {
        dev_ap6212_wifi_disconnect();
    }
}