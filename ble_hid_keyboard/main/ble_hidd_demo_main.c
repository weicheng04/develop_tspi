/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "esp_hidd_prf_api.h"
#include "esp_bt_defs.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "driver/gpio.h"
#include "hid_dev.h"

#include "dev_lcd.h"
#include "dev_pca9557.h"

#include "esp_lvgl_port.h"
#include "esp_random.h"

#include "ble_string_svc.h"
/**
 * Brief:
 * This example Implemented BLE HID device profile related functions, in which the HID device
 * has 4 Reports (1 is mouse, 2 is keyboard and LED, 3 is Consumer Devices, 4 is Vendor devices).
 * Users can choose different reports according to their own application scenarios.
 * BLE HID profile inheritance and USB HID class.
 */

/**
 * Note:
 * 1. Win10 does not support vendor report , So SUPPORT_REPORT_VENDOR is always set to FALSE, it defines in hidd_le_prf_int.h
 * 2. Update connection parameters are not allowed during iPhone HID encryption, slave turns
 * off the ability to automatically update connection parameters during encryption.
 * 3. After our HID device is connected, the iPhones write 1 to the Report Characteristic Configuration Descriptor,
 * even if the HID encryption is not completed. This should actually be written 1 after the HID encryption is completed.
 * we modify the permissions of the Report Characteristic Configuration Descriptor to `ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE_ENCRYPTED`.
 * if you got `GATT_INSUF_ENCRYPTION` error, please ignore.
 */

#define HID_DEMO_TAG "HID_DEMO"


static uint16_t hid_conn_id = 0;
static bool sec_conn = false;
static bool send_random_chars = false;   // flag to control random char sending
#define CHAR_DECLARATION_SIZE   (sizeof(uint8_t))

// LVGL UI elements
static lv_obj_t *btn_label = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *recv_label = NULL;
static lv_obj_t *recv_cont = NULL;

// Received text buffer
#define RECV_BUF_SIZE 1024
static char recv_text_buf[RECV_BUF_SIZE] = "";
static bool recv_first = true;

// Task handle for LVGL update task
static TaskHandle_t lvgl_update_task_handle = NULL;
// Flag: new string data pending LVGL update
static volatile bool recv_pending = false;

static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param);

#define HIDD_DEVICE_NAME            "HID"
static uint8_t hidd_service_uuid128[] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t hidd_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x0006, //slave connection min interval, Time = min_interval * 1.25 msec
    .max_interval = 0x0010, //slave connection max interval, Time = max_interval * 1.25 msec
    .appearance = 0x03c0,       //HID Generic,
    .manufacturer_len = 0,
    .p_manufacturer_data =  NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid = hidd_service_uuid128,
    .flag = 0x6,
};

static esp_ble_adv_params_t hidd_adv_params = {
    .adv_int_min        = 0x20,
    .adv_int_max        = 0x30,
    .adv_type           = ADV_TYPE_IND,
    .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
    //.peer_addr            =
    //.peer_addr_type       =
    .channel_map        = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/**
 * @brief Callback when a string is received via BLE string service.
 *        This runs in the BTC task context (limited stack), so we only
 *        buffer the data and notify the LVGL update task.
 */
static void on_ble_string_received(const uint8_t *data, uint16_t len)
{
    /* Clear placeholder on first reception */
    if (recv_first) {
        recv_text_buf[0] = '\0';
        recv_first = false;
    }

    size_t cur_len = strlen(recv_text_buf);

    /* If buffer would overflow, keep only the tail half */
    if (cur_len + len + 2 >= RECV_BUF_SIZE) {
        size_t keep = RECV_BUF_SIZE / 2;
        if (cur_len > keep) {
            memmove(recv_text_buf, recv_text_buf + cur_len - keep, keep);
            recv_text_buf[keep] = '\0';
            cur_len = keep;
        } else {
            recv_text_buf[0] = '\0';
            cur_len = 0;
        }
    }

    /* Append newline separator between messages */
    if (cur_len > 0) {
        recv_text_buf[cur_len++] = '\n';
    }

    /* Append new data */
    memcpy(recv_text_buf + cur_len, data, len);
    cur_len += len;
    recv_text_buf[cur_len] = '\0';

    ESP_LOGI(HID_DEMO_TAG, "BLE string received (%d bytes): %.*s", len, len, data);

    /* Signal the LVGL update task (don't touch LVGL from BTC task!) */
    recv_pending = true;
    if (lvgl_update_task_handle) {
        xTaskNotifyGive(lvgl_update_task_handle);
    }
}

/**
 * @brief Task that updates the LVGL display when new BLE data arrives.
 *        Runs with sufficient stack for LVGL operations.
 */
static void lvgl_recv_update_task(void *pvParameters)
{
    while (1) {
        /* Wait for notification from BLE callback */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (recv_pending) {
            recv_pending = false;
            if (lvgl_port_lock(portMAX_DELAY)) {
                lv_label_set_text(recv_label, recv_text_buf);
                if (recv_cont) {
                    lv_obj_scroll_to_y(recv_cont, LV_COORD_MAX, LV_ANIM_ON);
                }
                lvgl_port_unlock();
            }
        }
    }
}

static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch(event) {
        case ESP_HIDD_EVENT_REG_FINISH: {
            if (param->init_finish.state == ESP_HIDD_INIT_OK) {
                //esp_bd_addr_t rand_addr = {0x04,0x11,0x11,0x11,0x11,0x05};
                esp_ble_gap_set_device_name(HIDD_DEVICE_NAME);
                esp_ble_gap_config_adv_data(&hidd_adv_data);
            }
            break;
        }
        case ESP_BAT_EVENT_REG: {
            break;
        }
        case ESP_HIDD_EVENT_DEINIT_FINISH:
	     break;
		case ESP_HIDD_EVENT_BLE_CONNECT: {
            ESP_LOGI(HID_DEMO_TAG, "ESP_HIDD_EVENT_BLE_CONNECT");
            hid_conn_id = param->connect.conn_id;
            if (lvgl_port_lock(0)) {
                lv_label_set_text(status_label, "BLE: Connected");
                lvgl_port_unlock();
            }
            break;
        }
        case ESP_HIDD_EVENT_BLE_DISCONNECT: {
            sec_conn = false;
            send_random_chars = false;
            ESP_LOGI(HID_DEMO_TAG, "ESP_HIDD_EVENT_BLE_DISCONNECT");
            esp_ble_gap_start_advertising(&hidd_adv_params);
            if (lvgl_port_lock(0)) {
                lv_label_set_text(status_label, "BLE: Disconnected");
                lv_label_set_text(btn_label, LV_SYMBOL_PLAY " Start");
                lvgl_port_unlock();
            }
            break;
        }
        case ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT: {
            ESP_LOGI(HID_DEMO_TAG, "%s, ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT", __func__);
            ESP_LOG_BUFFER_HEX(HID_DEMO_TAG, param->vendor_write.data, param->vendor_write.length);
            break;
        }
        case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT: {
            ESP_LOGI(HID_DEMO_TAG, "ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT");
            ESP_LOG_BUFFER_HEX(HID_DEMO_TAG, param->led_write.data, param->led_write.length);
            break;
        }
        default:
            break;
    }
    return;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&hidd_adv_params);
        break;
     case ESP_GAP_BLE_SEC_REQ_EVT:
        for(int i = 0; i < ESP_BD_ADDR_LEN; i++) {
             ESP_LOGD(HID_DEMO_TAG, "%x:",param->ble_security.ble_req.bd_addr[i]);
        }
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
	 break;
     case ESP_GAP_BLE_AUTH_CMPL_EVT:
        esp_bd_addr_t bd_addr;
        memcpy(bd_addr, param->ble_security.auth_cmpl.bd_addr, sizeof(esp_bd_addr_t));
        ESP_LOGI(HID_DEMO_TAG, "remote BD_ADDR: %08x%04x",\
                (bd_addr[0] << 24) + (bd_addr[1] << 16) + (bd_addr[2] << 8) + bd_addr[3],
                (bd_addr[4] << 8) + bd_addr[5]);
        ESP_LOGI(HID_DEMO_TAG, "address type = %d", param->ble_security.auth_cmpl.addr_type);
        ESP_LOGI(HID_DEMO_TAG, "pair status = %s",param->ble_security.auth_cmpl.success ? "success" : "fail");
        if (param->ble_security.auth_cmpl.success) {
            sec_conn = true;
            ESP_LOGI(HID_DEMO_TAG, "secure connection established.");
            if (lvgl_port_lock(0)) {
                lv_label_set_text(status_label, "BLE: Paired");
                lvgl_port_unlock();
            }
        } else {
            ESP_LOGE(HID_DEMO_TAG, "pairing failed, reason = 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
        }
        break;
    default:
        break;
    }
}

/**
 * @brief LVGL button click callback - toggle random char sending
 */
static void btn_toggle_cb(lv_event_t *e)
{
    if (!sec_conn) {
        ESP_LOGW(HID_DEMO_TAG, "BLE not connected, cannot send keys");
        return;
    }
    send_random_chars = !send_random_chars;
    if (send_random_chars) {
        lv_label_set_text(btn_label, LV_SYMBOL_PAUSE " Stop");
        ESP_LOGI(HID_DEMO_TAG, "Start sending random chars");
    } else {
        lv_label_set_text(btn_label, LV_SYMBOL_PLAY " Start");
        ESP_LOGI(HID_DEMO_TAG, "Stop sending random chars");
    }
}

/**
 * @brief Task that sends random keyboard characters via BLE HID
 */
void hid_demo_task(void *pvParameters)
{
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    while (1) {
        if (sec_conn && send_random_chars) {
            // Generate a random key from a-z (HID_KEY_A=4 to HID_KEY_Z=29)
            uint8_t key_value = HID_KEY_A + (esp_random() % 26);
            ESP_LOGI(HID_DEMO_TAG, "Sending key: %c", 'a' + (key_value - HID_KEY_A));

            // Press key
            esp_hidd_send_keyboard_value(hid_conn_id, 0, &key_value, 1);
            vTaskDelay(50 / portTICK_PERIOD_MS);

            // Release key (send empty report)
            uint8_t key_release = 0;
            esp_hidd_send_keyboard_value(hid_conn_id, 0, &key_release, 1);
            vTaskDelay(200 / portTICK_PERIOD_MS);  // interval between keystrokes
        } else {
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}


void app_main(void)
{
    esp_err_t ret;

    // Initialize NVS.
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    dev_pca9557_init();
    dev_lcd_lvgl_init();

    if (lvgl_port_lock(0)) {
        // Title label
        lv_obj_t *title = lv_label_create(lv_scr_act());
        lv_label_set_text(title, "BLE HID Keyboard");
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

        // BLE connection status label
        status_label = lv_label_create(lv_scr_act());
        lv_label_set_text(status_label, "BLE: Advertising...");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0x888888), 0);
        lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 25);

        // Toggle button (smaller, repositioned higher)
        lv_obj_t *btn = lv_btn_create(lv_scr_act());
        lv_obj_set_size(btn, 140, 40);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 48);
        lv_obj_add_event_cb(btn, btn_toggle_cb, LV_EVENT_CLICKED, NULL);

        btn_label = lv_label_create(btn);
        lv_label_set_text(btn_label, LV_SYMBOL_PLAY " Start");
        lv_obj_center(btn_label);

        // "Received:" header label
        lv_obj_t *recv_header = lv_label_create(lv_scr_act());
        lv_label_set_text(recv_header, "Received:");
        lv_obj_set_style_text_color(recv_header, lv_color_hex(0x666666), 0);
        lv_obj_align(recv_header, LV_ALIGN_TOP_LEFT, 10, 95);

        // Scrollable container for received BLE text
        recv_cont = lv_obj_create(lv_scr_act());
        lv_obj_set_size(recv_cont, 300, 130);
        lv_obj_align(recv_cont, LV_ALIGN_BOTTOM_MID, 0, -5);
        lv_obj_set_style_bg_color(recv_cont, lv_color_hex(0xF0F0F0), 0);
        lv_obj_set_style_border_width(recv_cont, 1, 0);
        lv_obj_set_style_border_color(recv_cont, lv_color_hex(0xCCCCCC), 0);
        lv_obj_set_style_pad_all(recv_cont, 5, 0);
        lv_obj_set_scrollbar_mode(recv_cont, LV_SCROLLBAR_MODE_AUTO);

        recv_label = lv_label_create(recv_cont);
        lv_label_set_text(recv_label, "Waiting for data...");
        lv_label_set_long_mode(recv_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(recv_label, 280);

        lvgl_port_unlock();
    }

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(HID_DEMO_TAG, "%s initialize controller failed", __func__);
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(HID_DEMO_TAG, "%s enable controller failed", __func__);
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(HID_DEMO_TAG, "%s init bluedroid failed", __func__);
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(HID_DEMO_TAG, "%s init bluedroid failed", __func__);
        return;
    }

    if((ret = esp_hidd_profile_init()) != ESP_OK) {
        ESP_LOGE(HID_DEMO_TAG, "%s init bluedroid failed", __func__);
    }

    // Register the string receive callback (NUS service is created automatically by HID profile)
    ble_string_svc_register_recv_cb(on_ble_string_received);

    ///register the callback function to the gap module
    esp_ble_gap_register_callback(gap_event_handler);
    esp_hidd_register_callbacks(hidd_event_callback);

    /* set the security iocap & auth_req & key size & init key response key parameters to the stack*/
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;     //bonding with peer device after authentication
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;           //set the IO capability to No output No input
    uint8_t key_size = 16;      //the key size should be 7~16 bytes
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    /* If your BLE device act as a Slave, the init_key means you hope which types of key of the master should distribute to you,
    and the response key means which key you can distribute to the Master;
    If your BLE device act as a master, the response key means you hope which types of key of the slave should distribute to you,
    and the init key means which key you can distribute to the slave. */
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    xTaskCreate(&hid_demo_task, "hid_task", 4096, NULL, 5, NULL);
    xTaskCreate(&lvgl_recv_update_task, "lvgl_recv", 4096, NULL, 4, &lvgl_update_task_handle);
}
