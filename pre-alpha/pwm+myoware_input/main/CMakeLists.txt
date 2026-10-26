idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_ledc esp_driver_gpio esp_adc esp_pm esp_timer esp_hw_support)