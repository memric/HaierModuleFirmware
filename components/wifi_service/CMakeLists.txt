idf_component_register(SRCS "wifi_service.c"
    INCLUDE_DIRS "."
    REQUIRES esp_netif esp_wifi ota mqtt_service)
