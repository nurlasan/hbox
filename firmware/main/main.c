/*
 * Hbox на ESP32-S3 SuperMini. Пока — только тест кодировщиков (hbox-stf):
 * раз в минуту 10 с MP3 и 10 с AAC с микрофона, результат в консоль USB.
 */
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "bench.h"
#include "mic.h"

void app_main(void)
{
	esp_err_t err = mic_start();

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
