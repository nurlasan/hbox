/*
 * Хранилище записанных файлов до отправки.
 *
 * На microSD (store_sd.c): часовые файлы лежат до подтверждения сервером, переживают перезагрузку.
 *
 * Файл пишет recorder (create → write/pwrite → close), отправляет uplink (next → read → sent).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Длина файла; файлы начинаются на границе часа UTC. */
#define STORE_FILE_SECONDS 3600

struct store_file;

struct store_info {
	/* начало записи: время esp_timer (мкс) — реальное время подставляется при отправке */
	int64_t start_us;
	/* начало по UTC, если на момент записи время уже было известно, иначе 0 */
	int64_t start_utc;
	const char *ext; /* "mp3" / "m4a" */
	uint32_t size;
	uint32_t crc;
};

/* false — нет карты (запись не идёт; карту ищем снова при каждом новом файле) */
bool store_init(void);
/* Новый файл. Нет места — выбрасывается самый старый неотправленный (счётчик store_dropped). */
struct store_file *store_create(int64_t start_us, int64_t start_utc, const char *ext);
bool store_write(struct store_file *f, const void *data, size_t len);
/* Перезапись уже записанного (заголовок контейнера). */
bool store_pwrite(struct store_file *f, uint32_t offset, const void *data, size_t len);
/* Файл готов к отправке. */
void store_close(struct store_file *f);

/* Самый старый готовый к отправке файл или NULL; в info — размер и CRC-32. */
struct store_file *store_next(struct store_info *info);
size_t store_read(struct store_file *f, uint32_t offset, void *buf, size_t len);
/* Сервер подтвердил файл целиком — место освобождается. */
void store_sent(struct store_file *f);

uint32_t store_pending(void);
uint32_t store_dropped(void);
