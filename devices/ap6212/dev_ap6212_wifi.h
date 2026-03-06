#ifndef __DEV_AP6212_WIFI_H__
#define __DEV_AP6212_WIFI_H__

#include <stdint.h>

typedef enum
{
    AP6212_WIFI_STATUS_DISCONNECTED = 0,
    AP6212_WIFI_STATUS_CONNECTING,
    AP6212_WIFI_STATUS_CONNECTED,
    AP6212_WIFI_STATUS_ERROR,
} ap6212_wifi_status_t;

typedef struct
{
    char ssid[64];              // WiFi SSID
    char password[128];         // WiFi Password
    char interface[16];         // Network interface name (e.g., "wlan0")
    uint8_t max_retries;        // Maximum number of connection retries
    uint8_t retry_interval_sec; // Interval between connection retries in seconds
} ap6212_wifi_config_t;

typedef struct
{
    char ip_address[32];         // IP address assigned to the device
    char subnet_mask[32];        // Subnet mask
    char gateway[32];            // Default gateway
    char mac_address[32];        // MAC address of the WiFi interface
    int signal_strength;         // WiFi signal strength (RSSI)
    ap6212_wifi_status_t status; // Current WiFi connection status
} ap6212_wifi_info_t;

int dev_ap6212_wifi_init(void);
int dev_ap6212_wifi_connect(const ap6212_wifi_config_t *config);
int dev_ap6212_wifi_disconnect(void);
ap6212_wifi_status_t dev_ap6212_wifi_get_status(void);
int dev_ap6212_wifi_get_info(ap6212_wifi_info_t *info);
int dev_ap6212_wifi_is_connected(void);
void dev_ap6212_wifi_cleanup(void);

#endif /* __DEV_AP6212_WIFI_H__ */