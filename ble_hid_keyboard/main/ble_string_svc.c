/*
 * BLE String Receive Service implementation
 * Based on Nordic UART Service (NUS) UUIDs for wide compatibility
 * with Windows BLE tools (nRF Connect, BLE Terminal, etc.)
 *
 * This service is created under the same gatts_if as the HID profile,
 * so all events are dispatched through the HID profile's GATT callback.
 * The HID callback must call ble_string_svc_handle_create_evt() and
 * ble_string_svc_handle_write_evt() at the appropriate points.
 */

#include "ble_string_svc.h"
#include "esp_log.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include <string.h>

#define TAG "BLE_STRING_SVC"

/*
 * Nordic UART Service UUIDs (little-endian byte order)
 * Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * RX:      6E400002-B5A3-F393-E0A9-E50E24DCCA9E
 * TX:      6E400003-B5A3-F393-E0A9-E50E24DCCA9E
 */
static const uint8_t svc_uuid128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
};

static const uint8_t rx_char_uuid128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};

static const uint8_t tx_char_uuid128[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};

/// Attribute table indexes
enum {
    STR_SVC_IDX_SVC,

    STR_SVC_IDX_RX_CHAR,
    STR_SVC_IDX_RX_VAL,

    STR_SVC_IDX_TX_CHAR,
    STR_SVC_IDX_TX_VAL,
    STR_SVC_IDX_TX_CCC,

    STR_SVC_IDX_NB,
};

/// Standard GATT UUIDs
static const uint16_t primary_service_uuid       = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid  = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

/// Characteristic properties
static const uint8_t char_prop_write  = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t char_prop_notify = ESP_GATT_CHAR_PROP_BIT_NOTIFY;

/// CCC default value
static uint8_t tx_ccc[2] = {0x00, 0x00};

/// Handle table for the service
static uint16_t str_svc_handle_table[STR_SVC_IDX_NB];

/// GATT interface (shared with HID profile)
static esp_gatt_if_t str_svc_gatts_if = ESP_GATT_IF_NONE;

/// State tracking
static bool svc_created = false;
static bool svc_started = false;

/// User receive callback
static ble_string_recv_cb_t recv_cb = NULL;

/// Service attribute database
static const esp_gatts_attr_db_t str_svc_att_db[STR_SVC_IDX_NB] = {
    // ---- Service Declaration ----
    [STR_SVC_IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid,
         ESP_GATT_PERM_READ,
         sizeof(svc_uuid128), sizeof(svc_uuid128), (uint8_t *)svc_uuid128}
    },

    // ---- RX Characteristic (central writes here) ----
    [STR_SVC_IDX_RX_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
         ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_write}
    },
    [STR_SVC_IDX_RX_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)rx_char_uuid128,
         ESP_GATT_PERM_WRITE,
         BLE_STRING_MAX_LEN, 0, NULL}
    },

    // ---- TX Characteristic (device notifies here) ----
    [STR_SVC_IDX_TX_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid,
         ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t *)&char_prop_notify}
    },
    [STR_SVC_IDX_TX_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, (uint8_t *)tx_char_uuid128,
         0,
         BLE_STRING_MAX_LEN, 0, NULL}
    },
    [STR_SVC_IDX_TX_CCC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
         ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), sizeof(tx_ccc), (uint8_t *)tx_ccc}
    },
};


void ble_string_svc_register_recv_cb(ble_string_recv_cb_t cb)
{
    recv_cb = cb;
}


void ble_string_svc_create(esp_gatt_if_t gatts_if)
{
    if (svc_created) {
        ESP_LOGW(TAG, "Service already created");
        return;
    }
    str_svc_gatts_if = gatts_if;
    esp_err_t ret = esp_ble_gatts_create_attr_tab(str_svc_att_db, gatts_if, STR_SVC_IDX_NB, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create attr table: %s", esp_err_to_name(ret));
    } else {
        svc_created = true;
        ESP_LOGI(TAG, "Creating NUS attr table on gatts_if=%d", gatts_if);
    }
}


bool ble_string_svc_handle_create_evt(esp_ble_gatts_cb_param_t *param)
{
    if (param->add_attr_tab.num_handle != STR_SVC_IDX_NB || svc_started) {
        return false;  // Not our service
    }

    if (param->add_attr_tab.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "NUS attr table creation failed, status=%d", param->add_attr_tab.status);
        return true;  // It was ours but failed
    }

    memcpy(str_svc_handle_table, param->add_attr_tab.handles, sizeof(str_svc_handle_table));
    esp_ble_gatts_start_service(str_svc_handle_table[STR_SVC_IDX_SVC]);
    svc_started = true;
    ESP_LOGI(TAG, "NUS service started, svc_handle=%d, rx_handle=%d",
             str_svc_handle_table[STR_SVC_IDX_SVC],
             str_svc_handle_table[STR_SVC_IDX_RX_VAL]);
    return true;
}


bool ble_string_svc_handle_write_evt(esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    if (!svc_started) {
        return false;
    }

    if (param->write.handle == str_svc_handle_table[STR_SVC_IDX_RX_VAL]) {
        if (!param->write.is_prep) {
            ESP_LOGI(TAG, "Received %d bytes", param->write.len);
            if (recv_cb) {
                recv_cb(param->write.value, param->write.len);
            }
        }
        return true;
    }

    // Also handle CCC writes for the TX characteristic
    if (param->write.handle == str_svc_handle_table[STR_SVC_IDX_TX_CCC]) {
        return true;  // Handled by AUTO_RSP, just claim the event
    }

    return false;
}
