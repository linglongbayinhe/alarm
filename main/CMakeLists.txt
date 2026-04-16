idf_component_register(SRCS "blufi_example_main.c"
                            "blufi_security.c"
                            "blufi_init.c"
                            "display_lvgl.c"
                            "display_service.c"
                            "fonts/SourceHanSans_Normal_16.c"
                            "display/display_status_icon_renderer.c"
                            "display/display_weather_icon_renderer.c"
                            "status/status_presenter.c"
                            "weather/weather_presenter.c"
                            "weather/weather_mock_provider.c"
                            "time_service.c"
                            "rtc_service.c"
                            "wifi/wifi_signal_mapper.c"
                    PRIV_REQUIRES bt
                                  nvs_flash
                                  mbedtls
                                  esp_driver_i2c
                                  esp_driver_gpio
                                  esp_driver_spi
                                  esp_lcd
                                  spiffs
                                  lwip
                                  lvgl
                                  esp_lvgl_port
                    INCLUDE_DIRS "."
                                 "display"
                                 "status"
                                 "weather"
                                 "wifi")
