/*
 *   Created on: 2.07.2021
 *      Author: Valeriy Chudnikov
 */

#include "mqtt_service.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "lwip/dns.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_log.h"
#include "mqtt_client.h"

#ifndef CONFIG_MQTT_TOPIC
#define CONFIG_MQTT_TOPIC "HaierModule"
#endif

#ifndef CONFIG_MQTT_BROKER_HOST
#define CONFIG_MQTT_BROKER_HOST "127.0.0.1"
#endif

#ifndef CONFIG_MQTT_BROKER_PORT
#define CONFIG_MQTT_BROKER_PORT 1883
#endif

#ifndef CONFIG_MQTT_QOS_LEVEL
#define CONFIG_MQTT_QOS_LEVEL 0
#endif

static const char *TAG                      = "MQTT_SERVICE";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static uint8_t mqtt_client_connected        = 0; /*connection flag*/
char topic_string[128]                      = "";

#if CONFIG_MQTT_TLS_EN
extern const uint8_t mqtt_pem_start[] asm("_binary_mqtt_cert_pem_start");
extern const uint8_t mqtt_pem_end[] asm("_binary_mqtt_cert_pem_end");
#endif /* CONFIG_MQTT_TLS_EN */

static void mqtt_data_received(char *topic, int tlen, char *data, int dlen);

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);

    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;

    switch (event->event_id)
    {
    case MQTT_EVENT_CONNECTED:
        memset(topic_string, 0, sizeof(topic_string));
        strcpy(topic_string, CONFIG_MQTT_TOPIC);
        strcat(topic_string, "/set/#");

        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        mqtt_client_connected = 1;

        esp_mqtt_client_unsubscribe(client, topic_string);

        msg_id = esp_mqtt_client_subscribe(client, topic_string, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        mqtt_client_connected = 0;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "SUBSCRIBED to topic: %s", topic_string);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "Unsubscribed");
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "TOPIC: %.*s\tDATA: %.*s", event->topic_len, event->topic, event->data_len, event->data);
        mqtt_data_received(event->topic, event->topic_len, event->data, event->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        break;

    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void MQTT_AppStart(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker =
            {
                .address.hostname = CONFIG_MQTT_BROKER_HOST,
                .address.port     = CONFIG_MQTT_BROKER_PORT,
#if CONFIG_MQTT_TLS_EN
                .address.transport        = MQTT_TRANSPORT_OVER_SSL,
                .verification.certificate = (const char *) mqtt_pem_start
#endif /* CONFIG_MQTT_TLS_EN */
            },

#if CONFIG_MQTT_AUTH_EN
        .credentials = {
            .username = CONFIG_MQTT_USER, 
            .authentication.password = CONFIG_MQTT_PASSWORD
        }
#endif  /* CONFIG_MQTT_AUTH_EN */
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, mqtt_client);
    esp_mqtt_client_start(mqtt_client);
}

/**
 * @brief MQTT Initialization
 *
 * @return uint32_t
 */
uint32_t MQTT_ServiceInit(void)
{
    if (mqtt_client != NULL)
    {
        return MQTT_SERVICE_ERR;
    }

    esp_log_level_set(TAG, ESP_LOG_INFO);
    ESP_LOGI(TAG, "MQTT SERVICE Initialization");

    MQTT_AppStart();

    return MQTT_SERVICE_OK;
}

static void mqtt_data_received(char *topic, int tlen, char *data, int dlen)
{
    char *srch;

    if (dlen == 0 || dlen > 3)
    {
        return;
    }

    /*Reboot request*/
    srch = strstr(topic, "set/restart");
    if (srch != NULL)
    {
        ESP_LOGI(TAG, "Restart command received");
        esp_restart();
        return;
    }

    srch = strstr(topic, "set/");
    if (srch != NULL)
    {
    }
}

/**
 * @brief Message sending
 *
 * @param subt Subtopic
 * @param msg Message
 */
void MQTT_Send(const char *subt, const char *msg)
{
    int msg_id;
    char topic_msg[128] = {0};

    if (!mqtt_client || !mqtt_client_connected)
    {
        return;
    }

    if (strlen(subt) > 0)
    {
        sprintf(topic_msg, "%s/%s", CONFIG_MQTT_TOPIC, subt);
    }
    else
    {
        sprintf(topic_msg, "%s", CONFIG_MQTT_TOPIC);
    }

    msg_id = esp_mqtt_client_publish(mqtt_client, topic_msg, msg, strlen(msg), CONFIG_MQTT_QOS_LEVEL, 0);
    if (msg_id < 0)
    {
        ESP_LOGW(TAG, "publish error");
    }
    else
    {
        ESP_LOGI(TAG, "published, msg_id=%d: %s %s", msg_id, topic_msg, msg);
    }
}
