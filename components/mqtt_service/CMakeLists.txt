idf_component_register(SRCS "mqtt_service.c"
                    INCLUDE_DIRS "."
                    REQUIRES mqtt esp_netif esp_wifi
                    EMBED_TXTFILES "mqtt_cert.pem")
