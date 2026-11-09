#include "store.h"

#include <string.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/* 30 с при 128 кбит/с — 480 КБ; с запасом на контейнер и битрейт до 160 кбит/с */
#define SLOT_BYTES (STORE_FILE_SECONDS * 20000 + 16384)
#define SLOTS 3

enum state { FREE, RECORDING, READY, SENDING };

struct store_file {
	enum state state;
	uint8_t *data;
	struct store_info info;
	uint32_t seq; /* порядок создания: отправляем старые первыми */
};

static struct store_file slots[SLOTS];
static SemaphoreHandle_t lock;
static uint32_t seq, dropped;

bool store_init(void)
{
	lock = xSemaphoreCreateMutex();
	for (int i = 0; i < SLOTS; i++) {
		slots[i].data = heap_caps_malloc(SLOT_BYTES, MALLOC_CAP_SPIRAM);
		if (!slots[i].data) {
			return false;
		}
	}
	return true;
}

static struct store_file *oldest(enum state state)
{
	struct store_file *best = NULL;

	for (int i = 0; i < SLOTS; i++) {
		if (slots[i].state == state && (!best || slots[i].seq < best->seq)) {
			best = &slots[i];
		}
	}
	return best;
}

struct store_file *store_create(int64_t start_us, int64_t start_utc, const char *ext)
{
	struct store_file *f;

	xSemaphoreTake(lock, portMAX_DELAY);
	f = oldest(FREE);
	if (!f) {
		/* отправляемый не трогаем: uplink читает из него */
		f = oldest(READY);
		if (f) {
			dropped++;
		}
	}
	if (f) {
		f->state = RECORDING;
		f->seq = ++seq;
		f->info = (struct store_info){.start_us = start_us, .start_utc = start_utc, .ext = ext};
	}
	xSemaphoreGive(lock);
	return f;
}

bool store_write(struct store_file *f, const void *data, size_t len)
{
	if (f->info.size + len > SLOT_BYTES) {
		return false;
	}
	memcpy(f->data + f->info.size, data, len);
	f->info.size += len;
	return true;
}

bool store_pwrite(struct store_file *f, uint32_t offset, const void *data, size_t len)
{
	if (offset + len > f->info.size) {
		return false;
	}
	memcpy(f->data + offset, data, len);
	return true;
}

void store_close(struct store_file *f)
{
	xSemaphoreTake(lock, portMAX_DELAY);
	f->state = f->info.size ? READY : FREE;
	xSemaphoreGive(lock);
}

struct store_file *store_next(struct store_info *info)
{
	struct store_file *f;

	xSemaphoreTake(lock, portMAX_DELAY);
	f = oldest(SENDING);
	if (!f) {
		f = oldest(READY);
	}
	if (f) {
		f->state = SENDING;
	}
	xSemaphoreGive(lock);
	/* CRC — здесь, в задаче отправки: в записи это пауза, за которую теряется звук */
	if (f) {
		f->info.crc = esp_rom_crc32_le(0, f->data, f->info.size);
		*info = f->info;
	}
	return f;
}

size_t store_read(struct store_file *f, uint32_t offset, void *buf, size_t len)
{
	if (offset >= f->info.size) {
		return 0;
	}
	len = len < f->info.size - offset ? len : f->info.size - offset;
	memcpy(buf, f->data + offset, len);
	return len;
}

void store_sent(struct store_file *f)
{
	xSemaphoreTake(lock, portMAX_DELAY);
	f->state = FREE;
	xSemaphoreGive(lock);
}

uint32_t store_pending(void)
{
	uint32_t n = 0;

	xSemaphoreTake(lock, portMAX_DELAY);
	for (int i = 0; i < SLOTS; i++) {
		n += slots[i].state == READY || slots[i].state == SENDING;
	}
	xSemaphoreGive(lock);
	return n;
}

uint32_t store_dropped(void)
{
	return dropped;
}
