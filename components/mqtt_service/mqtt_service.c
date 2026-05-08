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

#include "cJSON.h"
#include "esp_log.h"
#include "haier.h"
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

#define MQTT_CHECK_STR(topic, check_str, len) strncmp(topic, check_str, len) == 0

static const char *TAG                      = "MQTT";
static esp_mqtt_client_handle_t mqtt_client = NULL;
static uint8_t mqtt_client_connected        = 0; /*connection flag*/
char topic_string[128]                      = "";
static TaskHandle_t mqtt_task               = NULL;

#if CONFIG_MQTT_TLS_EN
extern const uint8_t mqtt_pem_start[] asm("_binary_mqtt_cert_pem_start");
extern const uint8_t mqtt_pem_end[] asm("_binary_mqtt_cert_pem_end");
#endif /* CONFIG_MQTT_TLS_EN */

static void MQTT_DataReceived(char *topic, int tlen, char *data, int dlen);
static void MQTT_Task(void *arg);

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);

    esp_mqtt_event_handle_t event   = event_data;
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
        MQTT_DataReceived(event->topic, event->topic_len, event->data, event->data_len);
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
#else
                .address.transport = MQTT_TRANSPORT_OVER_TCP,
#endif /* CONFIG_MQTT_TLS_EN */
            },

#if CONFIG_MQTT_AUTH_EN
        .credentials = {.username = CONFIG_MQTT_USER, .authentication.password = CONFIG_MQTT_PASSWORD}
#endif  /* CONFIG_MQTT_AUTH_EN */
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, mqtt_client);
    esp_mqtt_client_start(mqtt_client);
}

/**
 * @brief MQTT Initialization
 *
 * @return uint32_t Error code
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

    xTaskCreate(MQTT_Task, "MQTT Task", 1024 * 4, NULL, 3, &mqtt_task);

    return MQTT_SERVICE_OK;
}

/**
 * @brief MQTT Service Deinitialization
 *
 * @return uint32_t Error code
 */
uint32_t MQTT_ServiceDeinit(void)
{
    if (mqtt_client != NULL)
    {
        if (mqtt_task != NULL)
        {
            vTaskDelete(mqtt_task);
            mqtt_task = NULL;
        }

        esp_mqtt_client_stop(mqtt_client);
        esp_mqtt_client_destroy(mqtt_client);
        mqtt_client = NULL;
    }

    return MQTT_SERVICE_OK;
}

static void MQTT_DataReceived(char *topic, int tlen, char *data, int dlen)
{
    int value = 0;
    char value_str[4];

    if (dlen == 0 || dlen > 3)
    {
        return;
    }

    memcpy(value_str, data, dlen);
    value_str[dlen] = '\0';

    /* Reboot request */
    if (MQTT_CHECK_STR(topic, "set/restart", tlen))
    {
        ESP_LOGI(TAG, "Restart command received");
        esp_restart();
        return;
    }

    /* Enable request */
    if (MQTT_CHECK_STR(topic, CONFIG_MQTT_TOPIC "/set/enable", tlen))
    {
        if (MQTT_CHECK_STR(value_str, "on", dlen) || MQTT_CHECK_STR(value_str, "1", dlen))
        {
            ESP_LOGI(TAG, "Enable request");
            Haier_Cmd(HAIER_ON, 1);
        }
        else if (MQTT_CHECK_STR(value_str, "off", dlen) || MQTT_CHECK_STR(value_str, "0", dlen))
        {
            ESP_LOGI(TAG, "Disable request");
            Haier_Cmd(HAIER_OFF, 0);
        }
    }
    else if (MQTT_CHECK_STR(topic, CONFIG_MQTT_TOPIC "/set/temp", tlen))
    {
        value = atoi(value_str);
        ESP_LOGI(TAG, "Temperature request. Value %d", value);
        if (value >= 16 && value < 30) // TODO
        {
            Haier_Cmd(HAIER_TEMP, (uint8_t) value);
        }
    }
    else if (MQTT_CHECK_STR(topic, CONFIG_MQTT_TOPIC "/set/mode", tlen))
    {
        value = atoi(value_str);
        ESP_LOGI(TAG, "Mode request. Value %d", value);
        if (value < 5) // TODO
        {
            Haier_Cmd(HAIER_MODE, (uint8_t) value);
        }
    }
    else if (MQTT_CHECK_STR(topic, CONFIG_MQTT_TOPIC "/set/fan", tlen))
    {
        value = atoi(value_str);
        ESP_LOGI(TAG, "Fan request. Value %d", value);
        if (value < 4) // TODO
        {
            Haier_Cmd(HAIER_FAN, (uint8_t) value);
        }
    }
}

/**
 * @brief Message sending
 *
 * @param subt Subtopic
 * @param msg Message
 *
 * @return uint32_t Error code
 */
uint32_t MQTT_Send(const char *subt, const char *msg)
{
    int msg_id;
    char topic_msg[128] = {0};
    uint32_t retval     = MQTT_SERVICE_ERR;

    if (!mqtt_client || !mqtt_client_connected)
    {
        return MQTT_SERVICE_ERR;
    }

    if (strlen(msg) == 0)
    {
        return MQTT_SERVICE_ERR;
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
        retval = MQTT_SERVICE_ERR;
    }
    else
    {
        ESP_LOGI(TAG, "published, msg_id=%d: %s %s", msg_id, topic_msg, msg);
        retval = MQTT_SERVICE_OK;
    }

    return retval;
}

static void MQTT_Task(void *arg)
{
    (void) arg;
#if CONFIG_HAIER_ENABLE
    haier_state_t haier_state;
#endif /* CONFIG_HAIER_ENABLE */
#if CONFIG_MQTT_LWT_EN
    TickType_t lwt_tick = xTaskGetTickCount();
#endif /* CONFIG_MQTT_LWT_EN */

    while (1)
    {
#if CONFIG_HAIER_ENABLE
        if (Haier_GetState(&haier_state, pdMS_TO_TICKS(10)) == 0)
        {
            /*Construct JSON method object*/
            cJSON *message = cJSON_CreateObject();

            if (message != NULL)
            {
                /* ON/OFF */
#if CONFIG_ENABLED_AS_STRING
                if (haier_state.en)
                {
                    cJSON_AddStringToObject(message, "Enabled", "on");
                }
                else
                {
                    cJSON_AddStringToObject(message, "Enabled", "off");
                }
#else
                cJSON_AddNumberToObject(message, "Enabled", haier_state.en);
#endif /* CONFIG_ENABLED_AS_STRING */

                /* Temperatures */
                cJSON_AddNumberToObject(message, "TempCurr", haier_state.temp_curr);
                cJSON_AddNumberToObject(message, "TempSet", haier_state.temp_set);

                /* Mode */
#if CONFIG_MODE_AS_STRING
                switch (haier_state.mode)
                {
                case 0:
                    cJSON_AddStringToObject(message, "Mode", "smart");
                    break;
                case 1:
                    cJSON_AddStringToObject(message, "Mode", "cool");
                    break;
                case 2:
                    cJSON_AddStringToObject(message, "Mode", "heat");
                    break;
                case 3:
                    cJSON_AddStringToObject(message, "Mode", "vent");
                    break;
                case 4:
                    cJSON_AddStringToObject(message, "Mode", "dry");
                    break;
                default:
                    cJSON_AddNumberToObject(message, "Mode", haier_state.mode);
                }
#else
                cJSON_AddNumberToObject(message, "Mode", haier_state.mode);
#endif /* CONFIG_MODE_AS_STRING */

                /* Fan speed */
#if CONFIG_FAN_SPEED_AS_STRING
                switch (haier_state.fan_speed)
                {
                case 0:
                    cJSON_AddStringToObject(message, "Fan", "max");
                    break;
                case 1:
                    cJSON_AddStringToObject(message, "Fan", "mid");
                    break;
                case 2:
                    cJSON_AddStringToObject(message, "Fan", "min");
                    break;
                case 3:
                    cJSON_AddStringToObject(message, "Fan", "auto");
                    break;
                default:
                    cJSON_AddNumberToObject(message, "Fan", haier_state.fan_speed);
                }
#else
                cJSON_AddNumberToObject(message, "Fan", haier_state.fan_speed);
#endif /* CONFIG_FAN_SPEED_AS_STRING */

                char *json_str = cJSON_PrintUnformatted(message);

                /* Send state topic */
                MQTT_Send("state", json_str);

                vPortFree(json_str);
                cJSON_Delete(message);
            }
        }
#else
        vTaskDelay(pdMS_TO_TICKS(1000));
#endif /* CONFIG_HAIER_ENABLE */

#if CONFIG_MQTT_LWT_EN
        if (lwt_tick + pdMS_TO_TICKS(CONFIG_MQTT_LWT_PERIOD) < xTaskGetTickCount())
        {
            /* Send state topic */
            MQTT_Send(CONFIG_MQTT_LWT_SUBTOPIC, CONFIG_MQTT_LWT_ONLINE_MSG);

            lwt_tick = xTaskGetTickCount();
        }
#endif /* CONFIG_MQTT_LWT_EN */
    }
}
