#ifndef __DEV_AP6212_BT_H__
#define __DEV_AP6212_BT_H__

#include <stdint.h>

/* ============================================================
 * AP6212 蓝牙 BLE 扫描驱动
 *
 * AP6212 通过 UART (/dev/ttyS1) 与主控通信，
 * 使用 hciattach 将 UART 注册为 HCI 接口，
 * 然后通过 BlueZ HCI Socket 进行 BLE 扫描。
 * ============================================================ */

/* BLE 扫描到的设备信息 */
typedef struct
{
    char address[18];  /* MAC 地址, e.g. "AA:BB:CC:DD:EE:FF" */
    char name[64];     /* 设备名称 (若广播中包含) */
    int8_t rssi;       /* 信号强度 (dBm) */
    uint8_t addr_type; /* 地址类型: 0=Public, 1=Random */
} ap6212_ble_device_t;

/* BLE 扫描结果回调函数
 * 每发现一个设备调用一次 */
typedef void (*ap6212_ble_scan_cb_t)(const ap6212_ble_device_t *device);

/**
 * @brief 初始化 AP6212 蓝牙模块
 *
 * 通过 hciattach 在 /dev/ttyS1 上初始化 HCI 接口，
 * 并启用 HCI 设备。
 *
 * @param uart_dev UART 设备路径, e.g. "/dev/ttyS1"
 * @param baudrate 波特率, 通常为 1500000
 * @return 0 成功, -1 失败
 */
int dev_ap6212_bt_init(const char *uart_dev, int baudrate);

/**
 * @brief 执行 BLE 扫描
 *
 * @param duration_sec 扫描持续时间(秒)
 * @param callback     每发现一个设备的回调(可为NULL, 则仅打印)
 * @return 发现的设备数量, -1 表示失败
 */
int dev_ap6212_bt_ble_scan(int duration_sec, ap6212_ble_scan_cb_t callback);

/**
 * @brief 关闭蓝牙模块, 清理资源
 */
void dev_ap6212_bt_cleanup(void);

#endif /* __DEV_AP6212_BT_H__ */