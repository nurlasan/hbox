/*
 * Время UTC: из сети через модем (AT+CCLK), запасной вариант — время сервера из /hbox/v1/config.
 * До первой установки времени запись идёт, а имя файла получает реальное время при отправке.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool clock_valid(void);
/* Текущее время UTC, с; 0 — ещё неизвестно. */
int64_t clock_now(void);
/* Время UTC момента esp_timer_get_time() == us; 0 — неизвестно. */
int64_t clock_at(int64_t us);
/* +CCLK: "26/11/10,14:05:09+20" (местное время и пояс в четвертях часа). */
bool clock_set_cclk(const char *line);
/* "2026-11-10T09:00:00Z" */
bool clock_set_iso(const char *s);
