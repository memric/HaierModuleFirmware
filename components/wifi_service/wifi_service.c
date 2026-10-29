/**
 * @file wifi_service.c
 * @brief WiFi service implementation for ESP32-C3
 * 
 * This module provides WiFi connectivity management including:
 * - WiFi initialization and configuration
 * - Automatic reconnection handling
 * - Connection status monitoring
 * - Integration with OTA and MQTT services
 */

#include "wifi_service.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#if CONFIG_MQTT_EN
#include "mqtt_service.h"
#endif /* CONFIG_MQTT_EN */
#if CONFIG_SYS_OTA_ENABLE
#include "ota.h"
#endif /* CONFIG_SYS_OTA_ENABLE */

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_FAIL_BIT         BIT1

#define WIFI_SERVICE_LOCK()   xSemaphoreTake(s_mutex, portMAX_DELAY)
#define WIFI_SERVICE_UNLOCK() xSemaphoreGive(s_mutex)

/* FreeRTOS event group to signal when we are connected */
static EventGroupHandle_t s_wifi_event_group;
static TimerHandle_t reconn_timer = NULL;
static SemaphoreHandle_t s_mutex  = NULL;
static int s_retry_num            = 0;
static const char *TAG            = "WIFI SERVICE";
static uint32_t reconn_cntr       = 0;
static bool connected             = false;
static char ip_addr[16]           = {'\0'}; /*! IP address string */

void ReconnectTimerCallback(TimerHandle_t xTimer);
void WifiServiceThread(void *argument);

/**
 * @brief WiFi and IP event handler
 * 
 * Handles WiFi events such as connection start, disconnection, and IP address acquisition.
 * Manages retry logic and event group signaling.
 * 
 * @param[in] arg      Opaque pointer (not used)
 * @param[in] event_base Event base (WIFI_EVENT or IP_EVENT)
 * @param[in] event_id   Specific event ID
 * @param[in] event_data Event-specific data
 */
static void WifiServiceEventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
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

        WIFI_SERVICE_LOCK();
        snprintf(ip_addr, sizeof(ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        ip_addr[sizeof(ip_addr) - 1] = '\0';
        WIFI_SERVICE_UNLOCK();

        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

/**
 * @brief Initialize WiFi service
 * 
 * Initializes WiFi subsystem including:
 * - Creates RTOS objects (timer, event group, semaphore)
 * - Configures network interface and WiFi stack
 * - Registers event handlers
 * - Starts WiFi connection thread
 * 
 * @return ESP_OK on success, ESP_ERR_NO_MEM if RTOS object creation fails
 */
esp_err_t WifiServiceInit(void)
{
    /* Create reconnect timer */
    reconn_timer       = xTimerCreate("Reconnect", pdMS_TO_TICKS(CONFIG_WIFI_RECONNECT_TIMER * 1000), pdFALSE, 0, ReconnectTimerCallback);
    /* Create event group for WiFi events */
    s_wifi_event_group = xEventGroupCreate();
    /* Create mutex semaphore */
    s_mutex            = xSemaphoreCreateMutex();

    if (reconn_timer == NULL || s_wifi_event_group == NULL || s_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed RTOS objects creation");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiServiceEventHandler, NULL, &instance_any_id
    ));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiServiceEventHandler, NULL, &instance_got_ip
    ));

    wifi_config_t wifi_config = {
        .sta = {
                .ssid               = CONFIG_WIFI_SSID,
                .password           = CONFIG_WIFI_PASSWORD,
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

    /* Start Control thread */
    if (xTaskCreate(WifiServiceThread, "WiFi Service", 1024 * 4, NULL, configMAX_PRIORITIES - 2, NULL) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create WiFi service thread");

        vEventGroupDelete(s_wifi_event_group);
        vSemaphoreDelete(s_mutex);
        xTimerDelete(reconn_timer, 0);

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/**
 * @brief Timer callback for WiFi reconnection
 * 
 * Called by the reconnection timer when it expires. Resets retry counter
 * and attempts to reconnect to the configured WiFi network.
 * 
 * @param[in] xTimer Handle to the timer that triggered the callback
 */
void ReconnectTimerCallback(TimerHandle_t xTimer)
{
    esp_wifi_connect();
    s_retry_num = 0;
}

/**
 * @brief WiFi service main thread
 * 
 * Monitors WiFi event group for connection/disconnection events.
 * On successful connection, initializes OTA and MQTT services.
 * On disconnection, starts the reconnection timer.
 * Runs continuously with periodic event checking.
 * 
 * @param[in] argument Thread argument (not used)
 */
void WifiServiceThread(void *argument)
{
    (void) argument;

    while (1)
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
                ESP_LOGI(TAG, "Connected to AP SSID:%s", CONFIG_WIFI_SSID);

                WIFI_SERVICE_LOCK();
                connected = true;
                reconn_cntr++;
                WIFI_SERVICE_UNLOCK();

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
                ESP_LOGI(TAG, "Failed to connect to SSID:%s", CONFIG_WIFI_SSID);
                ESP_LOGI(TAG, "Will try to reconnect 1 min later");

                WIFI_SERVICE_LOCK();
                connected = false;
                WIFI_SERVICE_UNLOCK();

                xTimerStart(reconn_timer, 0);
            }
        }
    }
}

/**
 * @brief Check WiFi connection status
 * 
 * Thread-safe function to determine if the device is currently connected
 * to the configured WiFi network and has obtained an IP address.
 * 
 * @return true if connected, false otherwise
 */
bool WifiServiceIsConnected(void)
{
    bool is_connected;

    WIFI_SERVICE_LOCK();
    is_connected = connected;
    WIFI_SERVICE_UNLOCK();

    return is_connected;
}

/**
 * @brief Get WiFi reconnection counter
 * 
 * Retrieves the total number of successful WiFi connections.
 * Thread-safe access to the internal reconnection counter.
 * 
 * @return Number of successful connections
 */
uint32_t WifiServiceGetReconnectCounter(void)
{
    uint32_t counter;

    WIFI_SERVICE_LOCK();
    counter = reconn_cntr;
    WIFI_SERVICE_UNLOCK();

    return counter;
}

/**
 * @brief Get current IP address
 * 
 * Copies the current IP address string into the provided buffer.
 * Thread-safe access to the internal IP address storage.
 * 
 * @param[out] buffer       Buffer to store the IP address string
 * @param[in]  buffer_size  Size of the provided buffer
 */
void WifiServiceGetIpAddress(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0)
    {
        return;
    }

    WIFI_SERVICE_LOCK();
    strncpy(buffer, ip_addr, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    WIFI_SERVICE_UNLOCK();
}
