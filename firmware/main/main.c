/*
 * Hbox на ESP32-S3 SuperMini. Пока — тесты:
 *   тест модема (hbox-xsg) при включении и раз в 10 минут;
 *   тест кодировщиков (hbox-stf) — по команде «b» в консоли USB.
 */
#include <stdio.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bench.h"
#include "mic.h"
#include "nettest.h"

#define NET_EVERY_MS (10 * 60 * 1000)

void app_main(void)
{
	usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
	esp_err_t err;

	/* с драйвером printf ждёт, пока ПК заберёт данные; без него при заполнении буфера вывод теряется */
	usb.tx_buffer_size = 4096;
	if (usb_serial_jtag_driver_install(&usb) == ESP_OK) {
		usb_serial_jtag_vfs_use_driver();
	}
	err = mic_start();
	if (err) {
		printf("HBOX ERROR mic %s\n", esp_err_to_name(err));
	}
	vTaskDelay(pdMS_TO_TICKS(3000));
	for (;;) {
		TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(NET_EVERY_MS);
		uint8_t c;

		nettest_run();
		printf("HBOX ready: next modem test in 10 min; 'n' - now, 'b' - codec bench\n");
		while (xTaskGetTickCount() < until) {
			if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(500)) != 1) {
				continue;
			}
			if (c == 'n') {
				break;
			}
			if (c == 'b' && !err) {
				bench_run();
			}
		}
	}
}
