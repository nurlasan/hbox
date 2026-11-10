/*
 * Hbox на ESP32-S3 SuperMini: запись звука в файлы и отправка на сервер через ML307R.
 *
 * Консоль USB (pcclient/monitor.bat):
 *   t <токен>  — токен устройства с сервера (HBOX_DEVICES в .env), хранится в NVS;
 *   s          — состояние сейчас (иначе раз в минуту).
 */
#include <stdio.h>
#include <string.h>
#include <driver/usb_serial_jtag.h>
#include <esp_app_desc.h>
#include <driver/usb_serial_jtag_vfs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "clock.h"
#include "mic.h"
#include "recorder.h"
#include "settings.h"
#include "store.h"
#include "uplink.h"

#define STATUS_EVERY_MS 60000

static struct settings settings;

static void status(void)
{
	struct recorder_stats rec;
	struct uplink_stats up;
	int64_t now = clock_now();

	recorder_get_stats(&rec);
	uplink_get_stats(&up);
	printf("HBOX STATUS time=%lld channel=%s rec=%d %s/%d files=%lu cpu=%lu.%lu%% overflows=%lu "
	       "rec_errors=%lu | pending=%lu dropped=%lu | up=%s csq=%d sent=%lu files %lu KB "
	       "last=%lu.%lu KB/s up_errors=%lu fw=%s\n",
	       now, settings.channel[0] ? settings.channel : "?", rec.recording, settings.format,
	       settings.bitrate / 1000, (unsigned long)rec.files, (unsigned long)rec.cpu10 / 10,
	       (unsigned long)rec.cpu10 % 10, (unsigned long)rec.overflows, (unsigned long)rec.errors,
	       (unsigned long)store_pending(), (unsigned long)store_dropped(), up.state, up.csq,
	       (unsigned long)up.sent_files, (unsigned long)up.sent_kb, (unsigned long)up.kbps10 / 10,
	       (unsigned long)up.kbps10 % 10, (unsigned long)up.errors,
	       esp_app_get_description()->version);
}

static void command(char *line)
{
	if (strncmp(line, "t ", 2) == 0 && strlen(line + 2) < sizeof(settings.token)) {
		/* меняем токен — канал узнаем от сервера заново */
		strcpy(settings.token, line + 2);
		settings.channel[0] = '\0';
		settings_save(&settings);
		printf("HBOX token saved\n");
	} else if (strcmp(line, "s") == 0) {
		status();
	}
}

void app_main(void)
{
	usb_serial_jtag_driver_config_t usb = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
	TickType_t next_status = xTaskGetTickCount() + pdMS_TO_TICKS(STATUS_EVERY_MS);
	char line[80];
	size_t n = 0;
	esp_err_t err;

	/* с драйвером printf ждёт, пока ПК заберёт данные; без него при заполнении буфера вывод теряется */
	usb.tx_buffer_size = 4096;
	if (usb_serial_jtag_driver_install(&usb) == ESP_OK) {
		usb_serial_jtag_vfs_use_driver();
	}
	settings_load(&settings);
	printf("HBOX start channel=%s token=%s format=%s bitrate=%d record=%d\n",
	       settings.channel[0] ? settings.channel : "?", settings.token[0] ? "set" : "none",
	       settings.format, settings.bitrate, settings.record);

	store_set_channel(settings.channel);
	if (!store_init()) {
		printf("HBOX ERROR store: no SD card\n");
	}
	err = mic_start();
	if (err) {
		printf("HBOX ERROR mic %s\n", esp_err_to_name(err));
	} else {
		recorder_start(settings.format, settings.bitrate, settings.record);
	}
	uplink_start(&settings);

	for (;;) {
		uint8_t c;

		if (usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(500)) == 1) {
			if (c == '\r' || c == '\n') {
				line[n] = '\0';
				if (n) {
					command(line);
				}
				n = 0;
			} else if (n < sizeof(line) - 1) {
				line[n++] = c;
			}
		}
		if (xTaskGetTickCount() >= next_status) {
			next_status += pdMS_TO_TICKS(STATUS_EVERY_MS);
			status();
		}
	}
}
