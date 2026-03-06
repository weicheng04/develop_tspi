/*
 * BLE String Receive Service (based on Nordic UART Service)
 * Allows a Windows PC (or any BLE central) to send strings to the device.
 *
 * Service UUID:        6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * RX Characteristic:   6E400002-B5A3-F393-E0A9-E50E24DCCA9E (write from central)
 * TX Characteristic:   6E400003-B5A3-F393-E0A9-E50E24DCCA9E (notify to central)
 *
 * This service is integrated into the existing HID GATT profile (sharing
 * the same gatts_if) so that it is always discoverable by BLE centrals
 * without needing a separate GATT app registration.
 *
 * Usage from Windows:
 *   1. Use nRF Connect / BLE Serial Terminal / custom Python script
 *   2. Connect to the "HID" device
 *   3. Discover services → find "Nordic UART Service"
 *   4. Write a UTF-8 string to the RX characteristic (UUID ending 0002)
 *   5. The string will appear on the device's LCD screen
 */

#ifndef __BLE_STRING_SVC_H__
#define __BLE_STRING_SVC_H__

#include "esp_gatts_api.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Max received string length per write
#define BLE_STRING_MAX_LEN      512

/**
 * @brief Callback type for received string data
 * @param data  Pointer to the received data bytes
 * @param len   Length of the received data
 */
typedef void (*ble_string_recv_cb_t)(const uint8_t *data, uint16_t len);

/**
 * @brief Register a callback for received string data
 * @param cb  The callback function
 */
void ble_string_svc_register_recv_cb(ble_string_recv_cb_t cb);

/**
 * @brief Create the NUS attribute table under the given GATT interface.
 *        Called from the HID profile after HID service is started.
 * @param gatts_if  The GATT server interface (same as HID profile's)
 */
void ble_string_svc_create(esp_gatt_if_t gatts_if);

/**
 * @brief Handle CREAT_ATTR_TAB_EVT. Checks if the event belongs to
 *        the NUS service and handles it.
 * @return true if this event was for the NUS service (caller should skip further handling)
 */
bool ble_string_svc_handle_create_evt(esp_ble_gatts_cb_param_t *param);

/**
 * @brief Handle WRITE_EVT. Checks if the write targets the NUS RX characteristic.
 * @return true if this event was for the NUS service (caller should skip further handling)
 */
bool ble_string_svc_handle_write_evt(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_STRING_SVC_H__ */
