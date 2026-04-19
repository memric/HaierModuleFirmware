#include "driver/gpio.h"
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
#include "driver/temperature_sensor.h"
#endif
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "haier.h"
#include "mqtt_service.h"
#include "nvs_flash.h"
#include "ota.h"
#include "telegram_bot.h"
#include <math.h>
#include <string.h>
#if CONFIG_MQTT_EN
#include "cJSON.h"
#endif

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define LED_PIN            GPIO_NUM_7

#define LED_ON()           gpio_set_level(LED_PIN, 1)
#define LED_OFF()          gpio_set_level(LED_PIN, 0)

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num           = 0;
static TimerHandle_t reconnTimer = NULL;
static const char *TAG           = "MAIN";

typedef struct
{
    float temp;
} sys_info_t;

void reconnectTimerCallback(TimerHandle_t xTimer);
int32_t SendSysInfo(sys_info_t *sys_info);

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < 5 /*ESP_MAXIMUM_RETRY*/)
        {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        }
        else
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "connect to the AP fail");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void app_main(void)
{
#if CONFIG_SYS_SENDING_PERIOD
    TickType_t sys_info_tick = xTaskGetTickCount();
#endif /* CONFIG_SYS_SENDING_PERIOD */

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if CONFIG_SYS_TEMP_SENSOR_ENABLE
    temperature_sensor_handle_t temp_sensor        = NULL;
    temperature_sensor_config_t temp_sensor_config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 50);
    ESP_ERROR_CHECK(temperature_sensor_install(&temp_sensor_config, &temp_sensor));

    ESP_LOGI(TAG, "Enable temperature sensor");
    ESP_ERROR_CHECK(temperature_sensor_enable(temp_sensor));
#endif /* CONFIG_SYS_TEMP_SENSOR_ENABLE */

    /* Init Haier interface */
    Haier_Init();

    /* Create reconnect timer */
    reconnTimer = xTimerCreate("Reconnect", 60 * 1000 / portTICK_PERIOD_MS, pdFALSE, 0, reconnectTimerCallback);

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    s_wifi_event_group     = xEventGroupCreate();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id)
    );
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip)
    );

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t sta_config = {
        .sta = {.ssid = CONFIG_ESP_WIFI_SSID, .password = CONFIG_ESP_WIFI_PASSWORD, .bssid_set = false}
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    gpio_config_t io_conf = {};
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    io_conf.mode          = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask  = (1ULL << LED_PIN);
    io_conf.pull_down_en  = 0;
    io_conf.pull_up_en    = 0;
    gpio_config(&io_conf);
    LED_OFF();

    while (true)
    {
        /* Check WiFi events */
        if (s_wifi_event_group)
        {
            EventBits_t bits = xEventGroupWaitBits(
                s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE, 300 / portTICK_PERIOD_MS
            );

            /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
             * happened. */
            if (bits & WIFI_CONNECTED_BIT)
            {
                ESP_LOGI(TAG, "Connected to AP SSID:%s", CONFIG_ESP_WIFI_SSID);
                LED_ON();

                /* Start OTA task */
                if (OTA_init() != 0)
                {
                    ESP_LOGE(TAG, "OTA initialization error");
                }

#if CONFIG_TG_EN
                /* Start Telegram bot */
                TeleBot_Init();
#endif /* CONFIG_TG_EN */

#if CONFIG_MQTT_EN
                /* Start MQTT service */
                if (MQTT_ServiceInit() != MQTT_SERVICE_OK)
                {
                    ESP_LOGE(TAG, "MQTT service initialization error");
                }
#endif /* CONFIG_MQTT_EN */
            }
            else if (bits & WIFI_FAIL_BIT)
            {
                ESP_LOGI(TAG, "Failed to connect to SSID:%s", CONFIG_ESP_WIFI_SSID);
                ESP_LOGI(TAG, "Will try to reconnect 1 min later");

                LED_OFF();

#if CONFIG_MQTT_EN
                /* Start MQTT service */
                MQTT_ServiceDeinit();
#endif /* CONFIG_MQTT_EN */

                xTimerStart(reconnTimer, 0);
            }
        }

#if CONFIG_SYS_SENDING_PERIOD
        /* System information sending */
        if (sys_info_tick + pdMS_TO_TICKS(CONFIG_SYS_SENDING_PERIOD) <
            xTaskGetTickCount()) // TODO add period to kconfig
        {
            sys_info_t sys_info = {.temp = -273.F};
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
            ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, &sys_info.temp));
#endif /* CONFIG_SYS_TEMP_SENSOR_ENABLE */

            SendSysInfo(&sys_info);
            sys_info_tick = xTaskGetTickCount();
        }
#endif /* CONFIG_SYS_SENDING_PERIOD */
    }
}

void reconnectTimerCallback(TimerHandle_t xTimer)
{
    esp_wifi_connect();
    s_retry_num = 0;
}

/**
 * @brief Sends JSON with system information
 *
 * @return int32_t
 */
int32_t SendSysInfo(sys_info_t *sys_info)
{
    int32_t retval = -1;

#if CONFIG_MQTT_EN

    /*Construct JSON method object*/
    cJSON *message                 = cJSON_CreateObjectReference(NULL);

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(message, "AppVer", app_desc->version);
    cJSON_AddStringToObject(message, "IdfVer", esp_get_idf_version());
    cJSON_AddNumberToObject(message, "LifeTime", pdTICKS_TO_MS(xTaskGetTickCount()) / 1000);
    cJSON_AddNumberToObject(message, "ResetReason", esp_reset_reason());
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
    cJSON_AddNumberToObject(message, "Temp", roundf(sys_info->temp));
#endif /* CONFIG_SYS_TEMP_SENSOR_ENABLE */

    char *msg_to_send = cJSON_PrintUnformatted(message);

    MQTT_Send("sys", msg_to_send);

    retval = strlen(msg_to_send);

    free(msg_to_send);

    cJSON_Delete(message);

#endif /* CONFIG_MQTT_EN */

    return retval;
}
