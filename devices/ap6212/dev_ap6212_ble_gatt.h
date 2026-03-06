#ifndef __DEV_AP6212_BLE_GATT_H__
#define __DEV_AP6212_BLE_GATT_H__

#include <stdint.h>

/* ============================================================
 * AP6212 BLE GATT 客户端驱动
 *
 * 用于连接 BLE 外设 (如 ESP32-S3),
 * 通过 GATT 协议实现双向数据收发。
 *
 * 底层使用 L2CAP BLE 套接字 + ATT 协议。
 * ============================================================ */

/* 最大发现数量 */
#define BLE_MAX_SERVICES 16
#define BLE_MAX_CHARS 32

/* GATT Characteristic 属性标志 */
#define BLE_PROP_BROADCAST 0x01
#define BLE_PROP_READ 0x02
#define BLE_PROP_WRITE_NO_RSP 0x04
#define BLE_PROP_WRITE 0x08
#define BLE_PROP_NOTIFY 0x10
#define BLE_PROP_INDICATE 0x20

/* UUID 类型 */
typedef struct
{
    uint8_t type; /* 0 = 16-bit, 1 = 128-bit */
    union
    {
        uint16_t uuid16;
        uint8_t uuid128[16];
    };
} ble_uuid_t;

/* GATT Service */
typedef struct
{
    uint16_t start_handle;
    uint16_t end_handle;
    ble_uuid_t uuid;
} ble_service_t;

/* GATT Characteristic */
typedef struct
{
    uint16_t decl_handle;  /* 声明句柄 */
    uint16_t value_handle; /* 值句柄 */
    uint8_t properties;    /* 属性 (R/W/N 等) */
    ble_uuid_t uuid;
} ble_characteristic_t;

/* 连接状态 */
typedef struct
{
    int connected;
    int l2cap_sock;
    char device_name[64];
    char device_addr[18];
    uint8_t addr_type;
    uint16_t mtu;

    /* 已发现的服务 */
    ble_service_t services[BLE_MAX_SERVICES];
    int service_count;

    /* 已发现的特征 */
    ble_characteristic_t chars[BLE_MAX_CHARS];
    int char_count;

    /* 自动检测的收发句柄 */
    uint16_t rx_handle;      /* 可写特征句柄 (向远端发数据) */
    uint16_t tx_handle;      /* 可通知特征句柄 (从远端收数据) */
    uint16_t tx_cccd_handle; /* CCCD 句柄 (用于启用通知) */
} ble_conn_t;

/* 通知回调函数 */
typedef void (*ble_notify_cb_t)(uint16_t handle, const uint8_t *data, int len);

/* ============================================================
 * 公有 API
 * ============================================================ */

/**
 * @brief 通过设备名称连接 BLE 外设
 *
 * 执行: 扫描 → 连接 → 发现服务/特征 → 启用通知
 *
 * @param device_name  目标设备名称, e.g. "HID"
 * @param scan_timeout_sec  扫描超时时间(秒)
 * @return 0 成功, -1 失败
 */
int dev_ap6212_ble_connect(const char *device_name, int scan_timeout_sec);

/**
 * @brief 断开 BLE 连接
 */
int dev_ap6212_ble_disconnect(void);

/**
 * @brief 向已连接的 BLE 设备发送数据
 *
 * 写入自动检测的可写特征 (rx_handle)
 *
 * @param data  发送数据
 * @param len   数据长度
 * @return 0 成功, -1 失败
 */
int dev_ap6212_ble_send(const uint8_t *data, int len);

/**
 * @brief 接收 BLE 设备发来的数据 (通知/指示)
 *
 * 阻塞等待直到收到数据或超时
 *
 * @param data      接收缓冲区
 * @param max_len   缓冲区最大长度
 * @param timeout_ms 超时时间(毫秒), 0 表示不等待
 * @return 接收字节数, 0 超时, -1 错误
 */
int dev_ap6212_ble_receive(uint8_t *data, int max_len, int timeout_ms);

/**
 * @brief 设置通知回调
 */
void dev_ap6212_ble_set_notify_callback(ble_notify_cb_t cb);

/**
 * @brief 获取连接状态
 */
const ble_conn_t *dev_ap6212_ble_get_state(void);

/**
 * @brief 检查是否已连接
 */
int dev_ap6212_ble_is_connected(void);

/**
 * @brief 向指定句柄写入数据 (Write Without Response)
 */
int dev_ap6212_ble_write(uint16_t handle, const uint8_t *data, int len);

/**
 * @brief 向指定句柄写入数据 (Write With Response, 等待 ATT_OP_WRITE_RSP)
 *
 * 适用于需要确认写入的特征, 比无响应写更可靠。
 *
 * @param handle  特征值句柄
 * @param data    数据
 * @param len     数据长度
 * @return 0 成功, -1 失败
 */
int dev_ap6212_ble_write_req(uint16_t handle, const uint8_t *data, int len);

/**
 * @brief 从指定句柄读取数据
 */
int dev_ap6212_ble_read(uint16_t handle, uint8_t *data, int max_len);

#endif /* __DEV_AP6212_BLE_GATT_H__ */
