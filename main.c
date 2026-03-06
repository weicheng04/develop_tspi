#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>

/* LVGL */
#include "lvgl.h"
#include "display/drm.h"
#include "indev/evdev.h"
#include "demos/lv_demos.h"

/* Application */
#include "lib_mqtt.h"
#include "dev_ap6212_wifi.h"
#include "dev_ap6212_bt.h"
#include "dev_ap6212_ble_gatt.h"

/* Screen resolution */
#define DISP_HOR_RES 480 // 水平分辨率
#define DISP_VER_RES 800 // 垂直分辨率

/* Touch read wrapper for 90° CCW landscape rotation */
void evdev_read_rotated(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    evdev_read(drv, data);
    /* After EVDEV_SWAP_AXES: x=touch_y(0..799), y=touch_x(0..479)
     * For CCW rotation we need to invert x: ui_x = (hor_res-1) - x */
    data->point.x = (drv->disp->driver->hor_res - 1) - data->point.x;
}

volatile sig_atomic_t running = 1;

/* ============================================================
 * Signal Handler
 * ============================================================ */
void signal_handler(int signum)
{
    printf("\nReceived Signal %d, Shutting Down...\n", signum);
    running = 0;
}

/* ============================================================
 * MQTT Callbacks
 * ============================================================ */
void on_message_received(const char *topic, const char *payload, int payload_len)
{
    printf("\n[MQTT] Message Received\n");
    printf("Topic: %s\n", topic);
    printf("Payload: %.*s\n", payload_len, payload);
}

void on_connection_lost(const char *cause)
{
    printf("\n[MQTT] Connection Lost: %s\n", cause);
}

/* ============================================================
 * BLE Notification Callback
 * ============================================================ */
void on_ble_notify(uint16_t handle, const uint8_t *data, int len)
{
    /* 尝试以文本打印, 如果是可打印字符 */
    int printable = 1;
    for (int i = 0; i < len; i++)
    {
        if (data[i] < 0x20 && data[i] != '\n' && data[i] != '\r' && data[i] != '\t')
        {
            printable = 0;
            break;
        }
    }

    if (printable && len > 0)
    {
        printf("[BLE Notify] handle=0x%04X: %.*s\n", handle, len, (const char *)data);
    }
    else
    {
        printf("[BLE Notify] handle=0x%04X (%d bytes):", handle, len);
        for (int i = 0; i < len && i < 32; i++)
            printf(" %02X", data[i]);
        if (len > 32)
            printf(" ...");
        printf("\n");
    }
}

/* ============================================================
 * BLE Communication Thread
 * ============================================================ */
void *ble_thread_func(void *arg)
{
    const char *target_name = (const char *)arg;

    printf("\n[BLE Thread] Starting, target: '%s'\n", target_name);

    /* 设置通知回调 */
    dev_ap6212_ble_set_notify_callback(on_ble_notify);

    /* 连接目标设备 (包含扫描 + L2CAP连接 + GATT发现) */
    if (dev_ap6212_ble_connect(target_name, 15) != 0)
    {
        printf("[BLE Thread] Failed to connect to '%s'\n", target_name);
        return NULL;
    }

    printf("[BLE Thread] Connected, starting data exchange...\n");

    int msg_count = 0;
    while (running && dev_ap6212_ble_is_connected())
    {
        /* 发送数据 */
        char send_buf[64];
        snprintf(send_buf, sizeof(send_buf), "RK3566 #%d", ++msg_count);

        printf("[BLE TX] Attempting to send: %s (len=%zu)\n",
               send_buf, strlen(send_buf));

        if (dev_ap6212_ble_send((uint8_t *)send_buf, strlen(send_buf)) == 0)
        {
            printf("[BLE TX] Sent successfully: %s\n", send_buf);
        }
        else
        {
            printf("[BLE TX] Send failed for msg #%d, checking connection...\n",
                   msg_count);
            if (!dev_ap6212_ble_is_connected())
            {
                printf("[BLE TX] Connection lost, exiting send loop\n");
                break;
            }
        }

        /* 接收数据 (等待 2 秒) */
        uint8_t recv_buf[256];
        int recv_len = dev_ap6212_ble_receive(recv_buf, sizeof(recv_buf) - 1, 2000);
        if (recv_len > 0)
        {
            recv_buf[recv_len] = '\0';
            printf("[BLE RX] Received (%d bytes): %s\n", recv_len, (char *)recv_buf);
        }
        else if (recv_len == 0)
        {
            printf("[BLE RX] No data received (timeout)\n");
        }
        else
        {
            printf("[BLE RX] Receive error (ret=%d), errno=%d: %s\n",
                   recv_len, errno, strerror(errno));
        }

        sleep(1);
    }

    dev_ap6212_ble_disconnect();
    printf("[BLE Thread] Done\n");

    return NULL;
}

/* ============================================================
 * MQTT Thread
 * ============================================================ */
void *mqtt_thread_func(void *arg)
{
    printf("\n[MQTT Thread] Starting...\n");

    if (!running)
    {
        return NULL;
    }

    mqtt_config_t mqtt_cfg = {
        .broker_address = "mqtt://139.9.0.201:1883",
        .client_id = "RK3566_Device",
        .username = "",
        .password = "",
        .keep_alive_interval = 20,
        .clean_session = 1,
        .time_out_ms = 10000,
    };

    lib_mqtt_set_message_callback(on_message_received);
    lib_mqtt_set_connlost_callback(on_connection_lost);

    if (lib_mqtt_connect(&mqtt_cfg) != 0)
    {
        printf("\n[Fatal Error] MQTT Connection Failed\n");
        return NULL;
    }

    if (lib_mqtt_subscribe("rk3566/subscribe", MQTT_QOS_0) != 0)
    {
        printf("\n[Warning] Subscription Failed\n");
    }

    printf("[MQTT Thread] Connected and Subscribed\n");

    while (running)
    {
        if (lib_mqtt_is_connected())
        {
            if (lib_mqtt_publish_str("rk3566/publish", "Hello I am tspi", MQTT_QOS_0, 0) == 0)
            {
                printf("[MQTT Thread] Published: Hello I am tspi\n");
            }
            else
            {
                printf("[MQTT Thread] Publish Failed\n");
            }
        }
        /* 每 3 秒发布一次 */
        for (int i = 0; i < 3 && running; i++)
        {
            sleep(1);
        }
    }

    printf("\n[MQTT Thread] Cleaning up...\n");
    lib_mqtt_cleanup();

    return NULL;
}

/* ============================================================
 * GIF Emoji Declarations & Cycling
 * ============================================================ */
LV_IMG_DECLARE(anger);
LV_IMG_DECLARE(close_eys_quick);
LV_IMG_DECLARE(close_eys_slow);
LV_IMG_DECLARE(disdain);
LV_IMG_DECLARE(excited);
LV_IMG_DECLARE(fear);
LV_IMG_DECLARE(left);
LV_IMG_DECLARE(right);
LV_IMG_DECLARE(sad);

static const lv_img_dsc_t *gif_list[] = {
    &anger,
    &close_eys_quick,
    &close_eys_slow,
    &disdain,
    &excited,
    &fear,
    &left,
    &right,
    &sad,
};
static const int gif_count = sizeof(gif_list) / sizeof(gif_list[0]);
static int gif_index = 0;
static lv_obj_t *gif_obj = NULL;

static void gif_cycle_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    gif_index = (gif_index + 1) % gif_count;
    lv_gif_set_src(gif_obj, gif_list[gif_index]);
    printf("[GIF] Switching to emoji %d/%d\n", gif_index + 1, gif_count);
}

/* ============================================================
 * LVGL Tick Thread (5ms tick)
 * ============================================================ */
void *lvgl_tick_thread_func(void *arg)
{
    while (running)
    {
        usleep(5000); /* 5ms */
        lv_tick_inc(5);
    }
    return NULL;
}

/* ============================================================
 * Main
 * ============================================================ */
int main(int argc, char *argv[])
{
    pthread_t mqtt_thread;
    pthread_t ble_thread;
    pthread_t lvgl_tick_thread;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* --- WiFi & MQTT Init --- */
    printf("[Main] Initializing WiFi and MQTT managers...\n");
    dev_ap6212_wifi_init();
    lib_mqtt_init();

    /* --- Bluetooth BLE Init & Connect to ESP32 --- */
    printf("[Main] Initializing Bluetooth BLE...\n");
    if (dev_ap6212_bt_init("/dev/ttyS1", 115200) == 0)
    {
        /* 在后台线程中连接 ESP32-S3 "HID" 设备并收发数据 */
        if (pthread_create(&ble_thread, NULL, ble_thread_func, (void *)"HID") != 0)
        {
            printf("[Warning] Failed to create BLE thread\n");
        }
    }
    else
    {
        printf("[Warning] Bluetooth init failed, skipping BLE connect\n");
    }

    ap6212_wifi_config_t wifi_cfg = {
        .ssid = "CMCC-qXfU",
        .password = "dbyavh7s",
        .interface = "wlan0",
        .max_retries = 15,
        .retry_interval_sec = 2,
    };
    if (dev_ap6212_wifi_connect(&wifi_cfg) != 0)
    {
        printf("\n[Fatal Error] WiFi Connection Failed\n");
        return 1;
    }

    ap6212_wifi_info_t wifi_info;
    if (dev_ap6212_wifi_get_info(&wifi_info) == 0)
    {
        printf("\n[WiFi Info]\n");
        printf("IP Address: %s\n", wifi_info.ip_address);
        printf("Subnet Mask: %s\n", wifi_info.subnet_mask);
        printf("Gateway: %s\n", wifi_info.gateway);
        printf("MAC Address: %s\n", wifi_info.mac_address);
        printf("Signal Strength: %d dBm\n", wifi_info.signal_strength);
    }

    if (pthread_create(&mqtt_thread, NULL, mqtt_thread_func, NULL) != 0)
    {
        printf("[Fatal Error] Failed to create MQTT thread\n");
        return 1;
    }

    /* --- LVGL Init --- */
    printf("[Main] Initializing LVGL...\n");
    lv_init();

    /* --- DRM Display Init --- */
    printf("[Main] Initializing DRM display (/dev/dri/card0)...\n");
    drm_init();

    /* Get actual display size from DRM */
    lv_coord_t drm_width, drm_height;
    uint32_t drm_dpi;
    drm_get_sizes(&drm_width, &drm_height, &drm_dpi);
    printf("[Main] DRM Display: %dx%d, DPI: %u\n", drm_width, drm_height, drm_dpi);

    /* Allocate draw buffers */
    uint32_t buf_size = drm_width * drm_height;
    lv_color_t *buf1 = (lv_color_t *)malloc(buf_size * sizeof(lv_color_t));
    lv_color_t *buf2 = (lv_color_t *)malloc(buf_size * sizeof(lv_color_t));
    if (!buf1 || !buf2)
    {
        printf("[Fatal Error] Failed to allocate display buffers\n");
        if (buf1)
            free(buf1);
        if (buf2)
            free(buf2);
        drm_exit();
        return 1;
    }

    /* Initialize display buffer */
    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_size);

    /* Register display driver */
    /* Physical panel is 480x800 (portrait), we rotate 90° to show 800x480 (landscape) */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = drm_height; /* Landscape: hor = physical ver (800) */
    disp_drv.ver_res = drm_width;  /* Landscape: ver = physical hor (480) */
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = drm_flush_rotated; /* Custom flush with 90° rotation */
    disp_drv.wait_cb = drm_wait_vsync;
    disp_drv.full_refresh = 1; /* Always flush full frame for rotation */

    lv_disp_drv_register(&disp_drv);

    /* --- Evdev Touchscreen Init --- */
    printf("[Main] Initializing touchscreen (/dev/input/event2)...\n");
    evdev_init();

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read_rotated;
    lv_indev_drv_register(&indev_drv);

    /* --- Start LVGL tick thread --- */
    if (pthread_create(&lvgl_tick_thread, NULL, lvgl_tick_thread_func, NULL) != 0)
    {
        printf("[Fatal Error] Failed to create LVGL tick thread\n");
        free(buf1);
        free(buf2);
        drm_exit();
        return 1;
    }

    /* --- Display GIF Emoji Cycle --- */
    printf("[Main] Displaying GIF emoji cycle...\n");

    /* Set black background */
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

    /* Create GIF object and show first emoji */
    gif_obj = lv_gif_create(lv_scr_act());
    lv_gif_set_src(gif_obj, gif_list[gif_index]);
    lv_obj_center(gif_obj);

    /* Timer to cycle through GIF emojis every 3 seconds */
    lv_timer_create(gif_cycle_timer_cb, 3000, NULL);

    /* --- Main Loop: handle LVGL tasks --- */
    printf("[Main] Entering main loop...\n");
    while (running)
    {
        lv_timer_handler();
        usleep(5000); /* 5ms */
    }

    /* --- Cleanup --- */
    printf("[Main] Shutting down...\n");

    pthread_join(mqtt_thread, NULL);
    pthread_join(ble_thread, NULL);
    running = 0;
    pthread_join(lvgl_tick_thread, NULL);

    dev_ap6212_ble_disconnect();
    free(buf1);
    free(buf2);
    drm_exit();
    dev_ap6212_wifi_cleanup();
    dev_ap6212_bt_cleanup();

    printf("[Main] Done.\n");
    return 0;
}