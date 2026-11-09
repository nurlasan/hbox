#include "recorder.h"

#include <stdio.h>
#include <string.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "clock.h"
#include "enc.h"
#include "mic.h"
#include "store.h"

#define FRAME_MAX 1152

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static char want_ext[4] = "mp3";
static int want_bitrate = 128000;
static bool want_record = true;
static struct recorder_stats stats;

void recorder_configure(const char *format, int bitrate, bool record)
{
	taskENTER_CRITICAL(&mux);
	strcpy(want_ext, enc_ext(format));
	want_bitrate = bitrate;
	want_record = record;
	taskEXIT_CRITICAL(&mux);
}

void recorder_get_stats(struct recorder_stats *st)
{
	taskENTER_CRITICAL(&mux);
	*st = stats;
	taskEXIT_CRITICAL(&mux);
	st->overflows = mic_overflows();
}

/* Пора закрывать файл: прошла граница периода UTC, а без времени — длительность. */
static bool due(int64_t start_us)
{
	int64_t now_us = esp_timer_get_time();
	int64_t start = clock_at(start_us);

	if (!start) {
		return now_us - start_us >= (int64_t)STORE_FILE_SECONDS * 1000000;
	}
	return clock_now() >= (start / STORE_FILE_SECONDS + 1) * STORE_FILE_SECONDS;
}

static void task(void *arg)
{
	static int16_t pcm[FRAME_MAX];
	struct store_file *f = NULL;
	int64_t start_us = 0, busy = 0, audio_us = 0;
	size_t frame = 0;

	for (;;) {
		char ext[4];
		int bitrate;
		bool record;

		taskENTER_CRITICAL(&mux);
		strcpy(ext, want_ext);
		bitrate = want_bitrate;
		record = want_record;
		stats.recording = f != NULL;
		taskEXIT_CRITICAL(&mux);

		if (!f) {
			if (!record) {
				vTaskDelay(pdMS_TO_TICKS(1000));
				continue;
			}
			start_us = esp_timer_get_time();
			f = store_create(start_us, clock_now(), ext);
			frame = f ? enc_start(f, ext, bitrate) : 0;
			if (!frame) {
				printf("HBOX REC ERROR start %s\n", f ? "encoder" : "store");
				if (f) {
					store_close(f);
					f = NULL;
				}
				stats.errors++;
				vTaskDelay(pdMS_TO_TICKS(1000));
				continue;
			}
			printf("HBOX REC new file %s %d\n", ext, bitrate);
		}

		bool fail = mic_read(pcm, frame) != ESP_OK;
		int64_t t0 = esp_timer_get_time();

		fail = fail || !enc_frame(pcm);
		busy += esp_timer_get_time() - t0;
		audio_us += (int64_t)frame * 1000000 / MIC_SAMPLE_RATE;
		if (audio_us >= 10000000) {
			taskENTER_CRITICAL(&mux);
			stats.cpu10 = busy * 1000 / audio_us;
			taskEXIT_CRITICAL(&mux);
			busy = audio_us = 0;
		}

		if (fail || !record || due(start_us)) {
			enc_finish();
			store_close(f);
			f = NULL;
			taskENTER_CRITICAL(&mux);
			stats.files++;
			stats.errors += fail;
			taskEXIT_CRITICAL(&mux);
		}
	}
}

void recorder_start(const char *format, int bitrate, bool record)
{
	recorder_configure(format, bitrate, record);
	/* ядро 1: модем и USB — на ядре 0; стек — под кодек AAC */
	xTaskCreatePinnedToCore(task, "rec", 40960, NULL, 10, NULL, 1);
}
