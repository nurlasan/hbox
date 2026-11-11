#include "clock.h"

#include <stdio.h>
#include <string.h>
#include <esp_timer.h>

/* UTC (мкс) в момент esp_timer == 0; 0 — время ещё не установлено */
static volatile int64_t epoch_us;
/* погрешность последней установки (мкс) и когда она была (esp_timer) */
static int64_t err_us, err_at;

/* Дни от 1970-01-01 по гражданской дате (алгоритм Хиннанта). */
static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;

	return era * 146097 + doe - 719468;
}

int64_t clock_utc(int y, int mo, int d, int h, int mi, int s)
{
	return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

/* Кварц ESP уходит не больше чем на 50 ppm: столько добавляем к погрешности за прошедшее время. */
static int64_t error_now(int64_t now)
{
	return err_us + (now - err_at) / 20000;
}

bool clock_set_iso(const char *s, int64_t sent_us, int64_t got_us)
{
	int y, mo, d, h, mi, sec, ms = -1;

	if (sscanf(s, "%d-%d-%dT%d:%d:%d.%3d", &y, &mo, &d, &h, &mi, &sec, &ms) < 6 ||
	    y < 2025 || y > 2037 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) {
		return false;
	}
	/* сервер ставит время где-то между отправкой запроса и приходом ответа: берём середину,
	 * погрешность — половина задержки (по 4G бывает и 10 с); без миллисекунд — ещё ±0.5 с */
	int64_t server_us = clock_utc(y, mo, d, h, mi, sec) * 1000000 + (ms >= 0 ? ms * 1000 : 500000);
	int64_t mid = sent_us + (got_us - sent_us) / 2;
	int64_t err = (got_us - sent_us) / 2 + (ms >= 0 ? 1000 : 500000);
	int64_t now = esp_timer_get_time();

	/* берём только образец точнее текущих часов, иначе медленный ответ сдвинет их на секунды */
	if (epoch_us && err >= error_now(now)) {
		return true;
	}
	int64_t epoch = server_us - mid;

	if (epoch_us) {
		printf("HBOX TIME %+lld ms (rtt %lld ms)\n", (long long)(epoch - epoch_us) / 1000,
		       (long long)(got_us - sent_us) / 1000);
	}
	epoch_us = epoch;
	err_us = err;
	err_at = now;
	return true;
}

bool clock_valid(void)
{
	return epoch_us != 0;
}

int64_t clock_at(int64_t us)
{
	return epoch_us ? (epoch_us + us) / 1000000 : 0;
}

int64_t clock_now(void)
{
	return clock_at(esp_timer_get_time());
}
