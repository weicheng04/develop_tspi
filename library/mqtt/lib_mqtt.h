#ifndef __LIB_MQTT_H__
#define __LIB_MQTT_H__

#include <stdint.h>

typedef enum
{
    MQTT_STATUS_DISCONNECTED = 0,
    MQTT_STATUS_CONNECTING,
    MQTT_STATUS_CONNECTED,
    MQTT_STATUS_ERROR,
} mqtt_status_t;

typedef enum
{
    MQTT_QOS_0 = 0,
    MQTT_QOS_1,
    MQTT_QOS_2,
} mqtt_qos_t;

typedef struct
{
    char broker_address[256];     // MQTT broker address
    char client_id[64];           // MQTT client ID
    char username[64];            // MQTT username (optional)
    char password[64];            // MQTT password (optional)
    uint32_t time_out_ms;         // Connection timeout in milliseconds
    uint16_t keep_alive_interval; // Keep-alive interval in seconds
    uint8_t clean_session;        // Clean session flag (0 or 1)
} mqtt_config_t;

typedef void (*mqtt_message_callback_t)(const char *topic, const char *payload, int payload_len);
typedef void (*mqtt_connlost_callback_t)(const char *cause);

int lib_mqtt_init(void);
int lib_mqtt_connect(const mqtt_config_t *config);
int lib_mqtt_publish(const char *topic, const void *payload, int payload_len, mqtt_qos_t qos, int retained);
int lib_mqtt_publish_str(const char *topic, const char *payload, mqtt_qos_t qos, int retained);
int lib_mqtt_subscribe(const char *topic, mqtt_qos_t qos);
int lib_mqtt_unsubscribe(const char *topic);
mqtt_status_t lib_mqtt_get_status(void);
int lib_mqtt_is_connected(void);
void lib_mqtt_set_message_callback(mqtt_message_callback_t callback);
void lib_mqtt_set_connlost_callback(mqtt_connlost_callback_t callback);
void lib_mqtt_cleanup(void);

#endif /* __LIB_MQTT_H__ */