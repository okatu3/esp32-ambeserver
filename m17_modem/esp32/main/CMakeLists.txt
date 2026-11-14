idf_component_register(SRCS "main.c" "es8388.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_psram esp_timer nvs_flash vfs m17core codec2)
