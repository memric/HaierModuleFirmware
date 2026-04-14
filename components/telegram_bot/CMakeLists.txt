idf_component_register(SRCS "telegram_bot.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp-tls esp_netif esp_wifi haier cjson
                    EMBED_TXTFILES "telegram_cert.pem")
