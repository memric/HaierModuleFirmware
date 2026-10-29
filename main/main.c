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
#include "wifi_service.h"

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

typedef struct
{
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
    float temp;           /*! SoC temperature */
#endif                    /* CONFIG_SYS_TEMP_SENSOR_ENABLE */
    char ip_addr[16];     /*! IP address string */
} sys_info_t;

static TimerHandle_t led_timer    = NULL;
static const char *TAG            = "MAIN";
static sys_info_t sys_info        = {
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
    .temp = -273.F,
#endif /* CONFIG_SYS_TEMP_SENSOR_ENABLE */
    .ip_addr     = {'\0'}
};

void ReconnectTimerCallback(TimerHandle_t xTimer);
void LedTimerCallback(TimerHandle_t xTimer);
int32_t SendSysInfo(sys_info_t *sys_info);

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
#if CONFIG_HAIER_ENABLE
    Haier_Init();
#endif /* CONFIG_HAIER_ENABLE */

    /* Create LED timer */
    led_timer             = xTimerCreate("LED", pdMS_TO_TICKS(10), pdFALSE, 0, LedTimerCallback);

    gpio_config_t io_conf = {};
    io_conf.intr_type     = GPIO_INTR_DISABLE;
    io_conf.mode          = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask  = (1ULL << LED_PIN);
    io_conf.pull_down_en  = 0;
    io_conf.pull_up_en    = 0;
    gpio_config(&io_conf);
    LED_OFF();

    esp_err_t wifi_ret = WifiServiceInit();
    if (wifi_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi service initialization failed: %s", esp_err_to_name(wifi_ret));

        // TODO
    }

    while (true)
    {
#if CONFIG_SYS_SENDING_PERIOD
        /* System information sending */
        if (WifiServiceIsConnected() &&
            (sys_info_tick + pdMS_TO_TICKS(CONFIG_SYS_SENDING_PERIOD) < xTaskGetTickCount()))
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

        vTaskDelay(pdMS_TO_TICKS(10));
    }
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
    cJSON_AddStringToObject(message, "app", app_desc->version);
    cJSON_AddStringToObject(message, "idf", esp_get_idf_version());

    /* Calculate lifetime in days, hours, minutes, and seconds */
    uint32_t lifetime_seconds = pdTICKS_TO_MS(xTaskGetTickCount()) / 1000;
    uint32_t days = lifetime_seconds / 86400;
    uint32_t hours = (lifetime_seconds % 86400) / 3600;
    uint32_t minutes = (lifetime_seconds % 3600) / 60;
    uint32_t seconds = lifetime_seconds % 60;
    char lifetime_str[32];
    snprintf(lifetime_str, sizeof(lifetime_str), "%lud %luh %lum %lus", days, hours, minutes, seconds);
    cJSON_AddStringToObject(message, "lifetime", lifetime_str);

    cJSON_AddNumberToObject(message, "recon", WifiServiceGetReconnectCounter());
#if CONFIG_SYS_TEMP_SENSOR_ENABLE
    cJSON_AddNumberToObject(message, "temp", roundf(sys_info->temp));
#endif /* CONFIG_SYS_TEMP_SENSOR_ENABLE */
    WifiServiceGetIpAddress(sys_info->ip_addr, sizeof(sys_info->ip_addr));
    cJSON_AddStringToObject(message, "ip", sys_info->ip_addr);

    /* Get WiFi RSSI */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK)
    {
        cJSON_AddNumberToObject(message, "rssi", ap_info.rssi);
    }
    else
    {
        cJSON_AddNumberToObject(message, "rssi", 0);
    }

    switch (esp_reset_reason())
    {
    case ESP_RST_POWERON:
        cJSON_AddStringToObject(message, "reset_reason", "poweron");
        break;
    case ESP_RST_SW:
        cJSON_AddStringToObject(message, "reset_reason", "software");
        break;
    case ESP_RST_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
        cJSON_AddStringToObject(message, "reset_reason", "watchdog");
        break;
    case ESP_RST_PANIC:
        cJSON_AddStringToObject(message, "reset_reason", "panic");
        break;
    default:
        cJSON_AddStringToObject(message, "reset_reason", "unknown");
        break;
    }

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
