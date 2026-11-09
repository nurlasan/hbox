/*
 * Hbox на ESP32-S3 SuperMini. Пока — только тест кодировщиков (hbox-stf):
 * раз в минуту 10 с MP3 и 10 с AAC с микрофона, результат в консоль USB.
 */
#include <stdio.h>
#include <driver/usb_serial_jtag.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bench.h"
#include "mic.h"

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
		return;
	}
	for (;;) {
		vTaskDelay(pdMS_TO_TICKS(3000));
		bench_run();
		vTaskDelay(pdMS_TO_TICKS(60000));
	}
}
