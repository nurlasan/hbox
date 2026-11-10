/*
 * Время UTC — с сервера (/hbox/v1/config, при каждом запросе настроек).
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
/* Гражданская дата UTC → секунды Unix. */
int64_t clock_utc(int y, int mo, int d, int h, int mi, int s);
/* "2026-11-10T09:00:00Z" */
bool clock_set_iso(const char *s);
