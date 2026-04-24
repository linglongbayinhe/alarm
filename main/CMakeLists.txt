idf_component_register(SRCS "blufi_example_main.c"
                            "audio_cache_service.c"
                            "audio_service.c"
                            "blufi_security.c"
                            "blufi_init.c"
                            "device_cloud_service.c"
                            "display_lvgl.c"
                            "display_service.c"
                            "fonts/SourceHanSans_Normal_16.c"
                            "playback_task_service.c"
                            "display/display_status_icon_renderer.c"
                            "display/display_weather_icon_renderer.c"
                            "status/status_presenter.c"
                            "storage_service.c"
                            "status_lvgl_image.c"
                            "weather/weather_presenter.c"
                            "weather/weather_device_service_provider.c"
                            "weather/weather_mock_provider.c"
                            "time_service.c"
                            "rtc_service.c"
                            "wifi/wifi_signal_mapper.c"
                            "ui_bridge.c"
                            "weather_lvgl_image.c"
                            "weather/icons/weather_clear_day_64.c"
                            "weather/icons/weather_clear_night_64.c"
                            "ui/ui.c"
                            "ui/screens.c"
                            "ui/styles.c"
                            "ui/images.c"
                            "ui/ui_font_source_han_sans_sc_normal_16.c"
                            "ui/ui_font_source_hans_sans_sc_normal_60.c"
                    PRIV_REQUIRES bt
                                  nvs_flash
                                  mbedtls
                                  esp_driver_i2c
                                  esp_driver_i2s
                                  esp_driver_gpio
                                  esp_driver_spi
                                  esp_lcd
                                  spiffs
                                  fatfs
                                  wear_levelling
                                  spi_flash
                                  esp_partition
                                  lwip
                                  esp_http_client
                                  esp-tls
                                  cjson
                                  lvgl
                                  esp_lvgl_port
                    INCLUDE_DIRS "."
                                 "ui"
                                 "display"
                                 "status"
                                 "weather"
                                 "wifi")

spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)
