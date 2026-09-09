idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio
                                esp_driver_spi
                                esp_lcd
                                esp_partition
                                spi_flash
                                fatfs
                                wear_levelling
                                esp_timer
                                esp_emote_gen_player)
