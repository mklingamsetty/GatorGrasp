idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_ledc esp_driver_gpio esp_pm nvs_flash esp_wifi esp_event esp_netif bt)