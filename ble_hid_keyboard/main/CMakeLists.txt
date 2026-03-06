idf_component_register(SRCS "ble_string_svc.c" "ble_hidd_demo_main.c"
                            "esp_hidd_prf_api.c"
                            "hid_dev.c"
                            "hid_device_le_prf.c"                            "ble_string_svc.c"                            "devices/dev_lcd.c"
                            "devices/dev_pca9557.c"
                    PRIV_REQUIRES bt nvs_flash esp_driver_gpio driver esp_lcd
                    INCLUDE_DIRS "." "devices")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-const-variable)
