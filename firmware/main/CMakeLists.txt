idf_component_register(SRCS main.c mic.c enc.c m4a.c store_ram.c recorder.c clock.c settings.c modem.c uplink.c
                       INCLUDE_DIRS .
                       EMBED_TXTFILES hbox_ca.pem
                       PRIV_REQUIRES esp_driver_i2s esp_driver_usb_serial_jtag esp_driver_uart esp_timer esp_rom
                                     nvs_flash shine)
