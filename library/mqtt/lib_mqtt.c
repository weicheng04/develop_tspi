#include "lib_mqtt.h"
#include "MQTTClient.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static MQTTClient client = NULL;
static mqtt_status_t mqtt_status = MQTT_STATUS_DISCONNECTED;
static mqtt_config_t mqtt_config;
static mqtt_message_callback_t message_callback = NULL;
static mqtt_connlost_callback_t connlost_callback = NULL;

/**
 * @brief 连接丢失回调函数，当MQTT连接丢失时被调用。
 *
 * @param context 用户定义的上下文（在此实现中未使用）。
 * @param cause 连接丢失的原因描述。
 */
static void internal_connlost(void *context, char *cause)
{
    printf("\n[MQTT] Connection Lost: %s\n", cause);
    mqtt_status = MQTT_STATUS_DISCONNECTED;

    if (connlost_callback != NULL)
    {
        connlost_callback(cause);
    }
}

/**
 * @brief 消息到达回调函数，当MQTT消息到达时被调用。
 *
 * @param context 用户定义的上下文（在此实现中未使用）。
 * @param topicName 消息主题名称。
 * @param topicLen 消息主题名称长度。
 * @param message 到达的MQTT消息结构体指针。
 *
 * @return 返回1表示消息已成功处理，返回0表示处理失败。
 */
static int internal_msgarrvd(void *context, char *topicName, int topicLen,
                             MQTTClient_message *message)
{
    if (message_callback != NULL)
    {
        message_callback(topicName, (const char *)message->payload, message->payloadlen);
    }
    else
    {
        printf("\n[MQTT] Message Arrived\n");
        printf("Topic: %s\n", topicName);
        printf("Message: %.*s\n", message->payloadlen, (char *)message->payload);
    }

    MQTTClient_freeMessage(&message);
    MQTTClient_free(topicName);
    return 1;
}

/**
 * @brief 初始化MQTT客户端库，设置默认配置和回调函数。
 *
 * @return 返回0表示初始化成功，返回非0表示初始化失败。
 */
int lib_mqtt_init(void)
{
    mqtt_status = MQTT_STATUS_DISCONNECTED;
    memset(&mqtt_config, 0, sizeof(mqtt_config_t));
    message_callback = NULL;
    connlost_callback = NULL;

    // 设置默认值
    mqtt_config.keep_alive_interval = 20;
    mqtt_config.clean_session = 1;
    mqtt_config.time_out_ms = 10000;

    return 0;
}

/**
 * @brief 设置MQTT消息到达回调函数。
 *
 * @param callback 用户定义的消息回调函数，当MQTT消息到达时被调用。
 */
void lib_mqtt_set_message_callback(mqtt_message_callback_t callback)
{
    message_callback = callback;
}

/**
 * @brief 设置MQTT连接丢失回调函数。
 *
 * @param callback 用户定义的连接丢失回调函数，当MQTT连接丢失时被调用。
 */
void lib_mqtt_set_connlost_callback(mqtt_connlost_callback_t callback)
{
    connlost_callback = callback;
}

/**
 * @brief MQTT消息交付完成回调函数，当MQTT消息交付完成时被调用。
 *
 * @param context 用户定义的上下文（在此实现中未使用）。
 * @param dt 消息交付令牌，标识已交付的消息。
 */
static void internal_delivered(void *context, MQTTClient_deliveryToken dt)
{
    // 目前不处理消息交付完成的回调，可以根据需要添加日志或其他处理逻辑
}

/**
 * @brief 连接到MQTT服务器，使用提供的配置参数进行连接。
 *
 * @param config 指向mqtt_config_t结构体的指针，包含连接所需的配置信息。
 *
 * @return 返回0表示连接成功，返回-1表示连接失败。
 */
int lib_mqtt_connect(const mqtt_config_t *config)
{
    if (config == NULL)
    {
        printf("[MQTT] Invalid Configuration\n");
        return -1;
    }

    // 复制配置
    memcpy(&mqtt_config, config, sizeof(mqtt_config_t));

    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;
    int rc;

    printf("\n=== Mqtt Connection ===\n");
    printf("Server: %s\n", config->broker_address);
    printf("Client ID: %s\n", config->client_id);
    printf("\n");

    mqtt_status = MQTT_STATUS_CONNECTING;

    // 创建MQTT客户端
    printf("[1/2] Create Mqtt Client...\n");
    rc = MQTTClient_create(&client, config->broker_address, config->client_id,
                           MQTTCLIENT_PERSISTENCE_NONE, NULL);

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf("[Error] Unable To Create Client, rc: %d\n", rc);
        mqtt_status = MQTT_STATUS_ERROR;
        return -1;
    }

    // 设置回调
    MQTTClient_setCallbacks(client, NULL, internal_connlost,
                            internal_msgarrvd, internal_delivered);

    // 连接到服务器
    printf("[2/2] Connect To Mqtt Server...\n");
    conn_opts.keepAliveInterval = config->keep_alive_interval;
    conn_opts.cleansession = config->clean_session;
    conn_opts.connectTimeout = config->time_out_ms / 1000;

    // 如果提供了用户名和密码则设置
    if (strlen(config->username) > 0)
    {
        conn_opts.username = config->username;
        conn_opts.password = config->password;
    }

    rc = MQTTClient_connect(client, &conn_opts);

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf("[Error] Connection Failed, rc: %d\n", rc);

        switch (rc)
        {
        case 1:
            printf("  Reason: Incorrect Protocol Version\n");
            break;
        case 2:
            printf("  Reason: Invalid Client ID\n");
            break;
        case 3:
            printf("  Reason: Server Unavailable\n");
            break;
        case 4:
            printf("  Reason: Incorrect Username Or Password\n");
            break;
        case 5:
            printf("  Reason: Unauthorized\n");
            break;
        default:
            printf("  Reason: Unknown Error\n");
            break;
        }

        mqtt_status = MQTT_STATUS_ERROR;
        MQTTClient_destroy(&client);
        client = NULL;
        return -1;
    }

    mqtt_status = MQTT_STATUS_CONNECTED;
    printf("[Success] Connected To Mqtt Server\n");

    return 0;
}

/**
 * @brief 断开与MQTT服务器的连接，并清理MQTT客户端资源。
 *
 * @return 返回0表示成功断开连接，返回-1表示断开连接失败。
 */
int lib_mqtt_disconnect(void)
{
    if (mqtt_status != MQTT_STATUS_CONNECTED || client == NULL)
    {
        printf("[MQTT] Not Connected\n");
        return -1;
    }

    MQTTClient_disconnect(client, 10000);
    MQTTClient_destroy(&client);
    client = NULL;
    mqtt_status = MQTT_STATUS_DISCONNECTED;

    printf("[MQTT] Disconnected\n");
    return 0;
}

/**
 * @brief 发布MQTT消息到指定主题。
 *
 * @param topic 发布的主题名称。
 * @param payload 消息负载数据指针。
 * @param payload_len 消息负载数据长度。
 * @param qos 消息的服务质量级别（0、1或2）。
 * @param retained 是否保留消息（0表示不保留，1表示保留）。
 *
 * @return 返回0表示消息发布成功，返回-1表示消息发布失败。
 */
int lib_mqtt_publish(const char *topic, const void *payload, int payload_len, mqtt_qos_t qos, int retained)
{
    if (client == NULL || mqtt_status != MQTT_STATUS_CONNECTED)
    {
        printf("[MQTT] Not Connected\n");
        return -1;
    }

    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    int rc;

    pubmsg.payload = (void *)payload;
    pubmsg.payloadlen = payload_len;
    pubmsg.qos = qos;
    pubmsg.retained = retained;

    rc = MQTTClient_publishMessage(client, topic, &pubmsg, &token);

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf("[Error] Failed To Publish, rc: %d\n", rc);
        return -1;
    }

    // 如果QoS大于0则等待完成
    if (qos > MQTT_QOS_0)
    {
        rc = MQTTClient_waitForCompletion(client, token, mqtt_config.time_out_ms);
        return (rc == MQTTCLIENT_SUCCESS) ? 0 : -1;
    }

    return 0;
}

/**
 * @brief 发布MQTT消息到指定主题，消息负载为字符串。
 *
 * @param topic 发布的主题名称。
 * @param payload 消息负载字符串。
 * @param qos 消息的服务质量级别（0、1或2）。
 * @param retained 是否保留消息（0表示不保留，1表示保留）。
 *
 * @return 返回0表示消息发布成功，返回-1表示消息发布失败。
 */
int lib_mqtt_publish_str(const char *topic, const char *payload, mqtt_qos_t qos, int retained)
{
    return lib_mqtt_publish(topic, payload, strlen(payload), qos, retained);
}

/**
 * @brief 订阅指定主题以接收消息。
 *
 * @param topic 订阅的主题名称。
 * @param qos 订阅的服务质量级别（0、1或2）。
 *
 * @return 返回0表示订阅成功，返回-1表示订阅失败。
 */
int lib_mqtt_subscribe(const char *topic, mqtt_qos_t qos)
{
    if (client == NULL || mqtt_status != MQTT_STATUS_CONNECTED)
    {
        printf("[MQTT] Not Connected\n");
        return -1;
    }

    int rc = MQTTClient_subscribe(client, topic, qos);

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf("[Error] Failed To Subscribe, rc: %d\n", rc);
        return -1;
    }

    return 0;
}

/**
 * @brief 取消订阅指定主题，不再接收该主题的消息。
 *
 * @param topic 取消订阅的主题名称。
 *
 * @return 返回0表示取消订阅成功，返回-1表示取消订阅失败。
 */
int lib_mqtt_unsubscribe(const char *topic)
{
    if (client == NULL || mqtt_status != MQTT_STATUS_CONNECTED)
    {
        printf("[MQTT] Not Connected\n");
        return -1;
    }

    int rc = MQTTClient_unsubscribe(client, topic);

    if (rc != MQTTCLIENT_SUCCESS)
    {
        printf("[Error] Failed To Unsubscribe, rc: %d\n", rc);
        return -1;
    }

    return 0;
}

/**
 * @brief 获取当前MQTT连接状态。
 *
 * @return 返回当前的mqtt_status_t枚举值，表示MQTT连接的状态。
 */
mqtt_status_t lib_mqtt_get_status(void)
{
    return mqtt_status;
}

/**
 * @brief 检查当前是否已连接到MQTT服务器。
 *
 * @return 返回1表示已连接，返回0表示未连接。
 */
int lib_mqtt_is_connected(void)
{
    return (mqtt_status == MQTT_STATUS_CONNECTED) ? 1 : 0;
}

/**
 * @brief 清理MQTT客户端资源，如果当前已连接则先断开连接。
 */
void lib_mqtt_cleanup(void)
{
    if (mqtt_status == MQTT_STATUS_CONNECTED)
    {
        lib_mqtt_disconnect();
    }
}