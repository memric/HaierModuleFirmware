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
#if CONFIG_SYS_OTA_ENABLE
#include "ota.h"
#endif /* CONFIG_SYS_OTA_ENABLE */
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
#define LED_BLINK()                                                                                                    \
    {                                                                                                                  \
        LED_ON();                                                                                                      \
        xTimerStart(led_timer, 0);                                                                                     \
    }
#define LED_OFF() gpio_set_level(LED_PIN, 0)

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num            = 0;
static TimerHandle_t reconn_timer = NULL;
static TimerHandle_t led_timer    = NULL;
static const char *TAG            = "MAIN";

typedef struct
{
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
    float temp;           /*! SoC temperature */
#endif                    /* CONFIG_SYS_TEMP_SENSOR_ENABLE */
    uint32_t reconn_cntr; /*! Reconnect counter */
    bool connected;       /*! Connected */
} sys_info_t;

void ReconnectTimerCallback(TimerHandle_t xTimer);
void LedTimerCallback(TimerHandle_t xTimer);
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
    sys_info_t sys_info = {.temp = -273.F, .reconn_cntr = 0, .connected = false};

    /* Initialize NVS */
    esp_err_t ret       = nvs_flash_init();
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
#if CONFIG_HAIER_ENABLE
    Haier_Init();
#endif /* CONFIG_HAIER_ENABLE */

    /* Create LED timer */
    led_timer          = xTimerCreate("LED", pdMS_TO_TICKS(10), pdFALSE, 0, LedTimerCallback);
    /* Create reconnect timer */
    reconn_timer       = xTimerCreate("Reconnect", pdMS_TO_TICKS(60 * 1000), pdFALSE, 0, ReconnectTimerCallback);

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

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

    wifi_config_t wifi_config = {
        .sta = {
                .ssid               = CONFIG_ESP_WIFI_SSID,
                .password           = CONFIG_ESP_WIFI_PASSWORD,
                /* Authmode threshold resets to WPA2 as default if password matches WPA2
                * standards (password len => 8). If you want to connect the device to
                * deprecated WEP/WPA networks, Please set the threshold value to
                * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format
                * matching to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
                */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK, // TODO to Kconfig
    // .sae_pwe_h2e        = ESP_WIFI_SAE_MODE,
    // .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
#ifdef CONFIG_ESP_WIFI_WPA3_COMPATIBLE_SUPPORT
            .disable_wpa3_compatible_mode = 0,
#endif
                },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
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
                s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(300)
            );

            /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
             * happened. */
            if (bits & WIFI_CONNECTED_BIT)
            {
                ESP_LOGI(TAG, "Connected to AP SSID:%s", CONFIG_ESP_WIFI_SSID);
                sys_info.connected = true;
                sys_info.reconn_cntr++;

#if CONFIG_SYS_OTA_ENABLE
                if (OTA_Deinit() != 0)
                {
                    ESP_LOGE(TAG, "OTA deinitialization error");
                }

                /* Start OTA task */
                if (OTA_Init() != 0)
                {
                    ESP_LOGE(TAG, "OTA initialization error");
                }
#endif /* CONFIG_SYS_OTA_ENABLE */

#if CONFIG_TG_EN
                /* Start Telegram bot */
                TeleBot_Init();
#endif /* CONFIG_TG_EN */

#if CONFIG_MQTT_EN
                MQTT_ServiceDeinit();

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

                sys_info.connected = false;

                xTimerStart(reconn_timer, 0);
            }
        }

#if CONFIG_SYS_SENDING_PERIOD
        /* System information sending */
        if (sys_info.connected && (sys_info_tick + pdMS_TO_TICKS(CONFIG_SYS_SENDING_PERIOD) < xTaskGetTickCount()))
        {
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
            ESP_ERROR_CHECK(temperature_sensor_get_celsius(temp_sensor, &sys_info.temp));
#endif /* CONFIG_SYS_TEMP_SENSOR_ENABLE */

            if (SendSysInfo(&sys_info) > 0)
            {
                LED_BLINK();
            }

            sys_info_tick = xTaskGetTickCount();
        }
#endif /* CONFIG_SYS_SENDING_PERIOD */
    }
}

void ReconnectTimerCallback(TimerHandle_t xTimer)
{
    esp_wifi_connect();
    s_retry_num = 0;
}

void LedTimerCallback(TimerHandle_t xTimer)
{
    LED_OFF();
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

    /* Construct JSON method object */
    cJSON *message = cJSON_CreateObject();
    if (message == NULL)
    {
        return -1;
    }

    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON_AddStringToObject(message, "AppVer", app_desc->version);
    cJSON_AddStringToObject(message, "IdfVer", esp_get_idf_version());
    cJSON_AddNumberToObject(message, "LifeTime", pdTICKS_TO_MS(xTaskGetTickCount()) / 1000);
    cJSON_AddNumberToObject(message, "ResetReason", esp_reset_reason());
    cJSON_AddNumberToObject(message, "ReconnCntr", sys_info->reconn_cntr);
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
    cJSON_AddNumberToObject(message, "Temp", roundf(sys_info->temp));
#endif /* CONFIG_SYS_TEMP_SENSOR_ENABLE */

    char *msg_to_send = cJSON_PrintUnformatted(message);

    if (MQTT_Send("sys", msg_to_send) == MQTT_SERVICE_OK)
    {
        retval = strlen(msg_to_send);
    }

    if (msg_to_send != NULL)
    {
        free(msg_to_send);
    }

    cJSON_Delete(message);
#endif /* CONFIG_MQTT_EN */

    return retval;
}
