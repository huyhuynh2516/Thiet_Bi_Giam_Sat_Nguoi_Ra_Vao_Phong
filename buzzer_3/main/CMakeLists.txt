idf_component_register(SRCS "main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_netif driver esp_event lwip
                    INCLUDE_DIRS "")
