idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash esp_adc esp_driver_ledc esp_timer)