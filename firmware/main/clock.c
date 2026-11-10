#include "clock.h"

#include <stdio.h>
#include <string.h>
#include <esp_timer.h>

/* UTC в момент esp_timer == 0; 0 — время ещё не установлено */
static volatile int64_t epoch_at_boot;

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

static bool set(int y, int mo, int d, int h, int mi, int s, int offset_s)
{
	if (y < 2025 || y > 2037 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
		return false;
	}
	int64_t utc = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s - offset_s;
	int64_t epoch = utc - esp_timer_get_time() / 1000000;

	/* время сервера — с точностью до секунды: мелкие поправки не берём, иначе часы дёргаются
	 * на ±1 с и у границы периода получается лишний файл длиной в секунду */
	if (!epoch_at_boot || epoch - epoch_at_boot >= 2 || epoch_at_boot - epoch >= 2) {
		epoch_at_boot = epoch;
	}
	return true;
}

bool clock_valid(void)
{
	return epoch_at_boot != 0;
}

int64_t clock_at(int64_t us)
{
	return epoch_at_boot ? epoch_at_boot + us / 1000000 : 0;
}

int64_t clock_now(void)
{
	return clock_at(esp_timer_get_time());
}

bool clock_set_iso(const char *s)
{
	int y, mo, d, h, mi, sec;

	return sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6 &&
	       set(y, mo, d, h, mi, sec, 0);
}
