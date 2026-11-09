idf_component_register(SRCS main.c mic.c bench.c m4a.c modem.c nettest.c
                       INCLUDE_DIRS .
                       PRIV_REQUIRES esp_driver_i2s esp_driver_usb_serial_jtag esp_driver_uart esp_timer esp_rom shine)
