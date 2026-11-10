/*
 * Хранилище на microSD (модуль YS-41 по SPI): часовые файлы лежат на карте до подтверждения сервером.
 *
 * Запись карты бывает медленной (до сотен мс на кадр FAT), а у микрофона буфер ~0,5 с — поэтому
 * recorder кладёт данные в кольцо в PSRAM, а на карту их пишет отдельная задача.
 *
 * Имена на карте: <UTC начала>.<ext>; пока пишется — с .part; если время начала ещё неизвестно —
 * u<номер>.<ext>, переименовывается, как только время придёт. После перезагрузки .part от mp3
 * отправляется как есть (кадры MP3 независимы), m4a без moov — пока выбрасывается (hbox-5nq).
 */
#include "store.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <driver/sdspi_host.h>
#include <driver/spi_common.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdmmc_cmd.h>

#include "clock.h"

#define PIN_CS 10
#define PIN_SCK 11
#define PIN_MOSI 8
#define PIN_MISO 9

#define MOUNT "/sd"
#define SD_DIR MOUNT "/hbox"
#define FILES_MAX 256
/* 1 МБ — минута звука и весь moov часового m4a: запись не ждёт карту даже при её паузах */
#define RING_BYTES (1024 * 1024)
#define STAGE_BYTES 4096
/* место под следующий файл (160 кбит/с за период) с запасом */
#define KEEP_FREE ((uint64_t)STORE_FILE_SECONDS * 20000 + (16 << 20))

enum state { FREE, RECORDING, READY, SENDING };
enum op_type { OP_CREATE, OP_WRITE, OP_PWRITE, OP_CLOSE };

struct store_file {
	enum state state;
	struct store_info info;
	char ext[4];
	bool crc_known, failed;
	uint32_t seq;
};

struct op {
	uint8_t type;
	struct store_file *f;
	uint32_t offset;
	uint8_t data[];
};

static struct store_file files[FILES_MAX];
static SemaphoreHandle_t lock;
static RingbufHandle_t ring;
static sdmmc_card_t *card;
static uint32_t seq, dropped;
/* последняя ошибка карты: после неё новый файл — не раньше чем через FAIL_PAUSE_US */
static volatile int64_t failed_us;
#define FAIL_PAUSE_US 10000000LL
/* запись: копится по STAGE_BYTES и одним куском уходит в кольцо */
static struct {
	struct op op;
	uint8_t data[STAGE_BYTES];
} stage;
static size_t stage_len;
/* чтение для отправки: файл держим открытым между вызовами */
static FILE *rd;
static struct store_file *rd_f;
static uint8_t *crc_buf;

static void path_of(const struct store_file *f, bool part, char *out, size_t cap)
{
	if (f->info.start_utc) {
		snprintf(out, cap, SD_DIR "/%lld.%s%s", f->info.start_utc, f->ext, part ? ".part" : "");
	} else {
		snprintf(out, cap, SD_DIR "/u%lu.%s%s", (unsigned long)f->seq, f->ext, part ? ".part" : "");
	}
}

static bool mount(void)
{
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
	esp_vfs_fat_sdmmc_mount_config_t mc = {.max_files = 4, .allocation_unit_size = 32 * 1024};
	spi_bus_config_t bus = {.mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCK,
				.quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 16384};
	static bool bus_ready;
	esp_err_t err;

	if (card) {
		return true;
	}
	if (!bus_ready) {
		err = spi_bus_initialize(host.slot, &bus, SPI_DMA_CH_AUTO);
		if (err) {
			printf("HBOX SD spi %s\n", esp_err_to_name(err));
			return false;
		}
		bus_ready = true;
	}
	/* 10 МГц — с запасом для проводов и преобразователя уровней на модуле; поток записи ~16 КБ/с */
	host.max_freq_khz = 10000;
	dev.gpio_cs = PIN_CS;
	dev.host_id = host.slot;
	err = esp_vfs_fat_sdspi_mount(MOUNT, &host, &dev, &mc, &card);
	if (err) {
		printf("HBOX SD no card (%s)\n", esp_err_to_name(err));
		card = NULL;
		return false;
	}
	mkdir(SD_DIR, 0775);
	printf("HBOX SD %s %llu MB\n", card->cid.name,
	       (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
	return true;
}

static uint64_t free_bytes(void)
{
	uint64_t total = 0, free = 0;

	esp_vfs_fat_info(MOUNT, &total, &free);
	return free;
}

static struct store_file *oldest(enum state state)
{
	struct store_file *best = NULL;

	for (int i = 0; i < FILES_MAX; i++) {
		if (files[i].state == state && (!best || files[i].seq < best->seq)) {
			best = &files[i];
		}
	}
	return best;
}

/* Выбросить самый старый неотправленный файл. Под lock. */
static bool drop_oldest(void)
{
	struct store_file *f = oldest(READY);
	char path[48];

	if (!f) {
		return false;
	}
	path_of(f, false, path, sizeof(path));
	unlink(path);
	printf("HBOX SD dropped %s\n", path);
	f->state = FREE;
	dropped++;
	return true;
}

/* Время начала пришло позже, чем файл начался: переименовать u<номер> в <UTC>. */
static void name_late(struct store_file *f)
{
	char from[48], to[48];
	int64_t t = f->info.start_utc ? 0 : clock_at(f->info.start_us);

	if (!t) {
		return;
	}
	path_of(f, false, from, sizeof(from));
	f->info.start_utc = t;
	path_of(f, false, to, sizeof(to));
	rename(from, to);
}

/* --- задача записи на карту --- */

static void writer(void *arg)
{
	FILE *out = NULL;
	char path[48], final[48];

	for (;;) {
		size_t len;
		struct op *op = xRingbufferReceive(ring, &len, portMAX_DELAY);
		struct store_file *f = op->f;
		size_t n = len - sizeof(*op);

		switch (op->type) {
		case OP_CREATE:
			xSemaphoreTake(lock, portMAX_DELAY);
			while (free_bytes() < KEEP_FREE && drop_oldest()) {
			}
			xSemaphoreGive(lock);
			path_of(f, true, path, sizeof(path));
			out = fopen(path, "wb");
			f->failed = !out;
			if (!out) {
				failed_us = esp_timer_get_time();
				printf("HBOX SD ERROR create %s\n", path);
			}
			break;
		case OP_WRITE:
			if (out && fwrite(op->data, 1, n, out) == n) {
				f->info.size += n;
			} else if (!f->failed) {
				f->failed = true;
				failed_us = esp_timer_get_time();
				printf("HBOX SD ERROR write %s\n", path);
			}
			break;
		case OP_PWRITE:
			if (!out || fseek(out, op->offset, SEEK_SET) || fwrite(op->data, 1, n, out) != n ||
			    fseek(out, 0, SEEK_END)) {
				f->failed = true;
			}
			break;
		case OP_CLOSE:
			if (out) {
				fclose(out);
				out = NULL;
			}
			path_of(f, true, path, sizeof(path));
			xSemaphoreTake(lock, portMAX_DELAY);
			if (!f->info.start_utc) {
				f->info.start_utc = clock_at(f->info.start_us);
			}
			path_of(f, false, final, sizeof(final));
			if (f->info.size && rename(path, final) == 0) {
				f->state = READY;
			} else {
				unlink(path);
				f->state = FREE;
			}
			xSemaphoreGive(lock);
			break;
		}
		vRingbufferReturnItem(ring, op);
	}
}

/* В кольцо; ждём не дольше 2 с — карта зависла, дальше данные теряются (ошибка записи). */
static bool send_op(struct op *op, size_t data_len)
{
	return xRingbufferSend(ring, op, sizeof(*op) + data_len, pdMS_TO_TICKS(2000)) == pdTRUE;
}

static bool flush_stage(void)
{
	bool ok = true;

	if (stage_len) {
		stage.op.type = OP_WRITE;
		ok = send_op(&stage.op, stage_len);
		stage_len = 0;
	}
	return ok;
}

/* --- при включении: что осталось на карте --- */

static void scan(void)
{
	DIR *d = opendir(SD_DIR);
	struct dirent *e;
	int n = 0;

	if (!d) {
		return;
	}
	while ((e = readdir(d)) && n < FILES_MAX) {
		char path[300], ext[8] = "", part[8] = "";
		long long utc = 0;
		struct stat st;

		snprintf(path, sizeof(path), SD_DIR "/%s", e->d_name);
		if (sscanf(e->d_name, "%lld.%3[a-z0-9].%5s", &utc, ext, part) < 2 || utc <= 0 ||
		    (strcmp(ext, "mp3") && strcmp(ext, "m4a")) || (part[0] && strcmp(part, "part")) ||
		    (part[0] && strcmp(ext, "m4a") == 0) || stat(path, &st) || !st.st_size) {
			/* u<номер> — время так и не узнали; m4a без moov; мусор */
			printf("HBOX SD removing %s\n", e->d_name);
			unlink(path);
			continue;
		}
		struct store_file *f = &files[n++];

		*f = (struct store_file){.state = READY};
		strcpy(f->ext, ext);
		f->info = (struct store_info){.start_utc = utc, .ext = f->ext, .size = st.st_size};
		if (part[0]) {
			/* mp3 оборвался при выключении — отправляем, что успело записаться */
			char final[48];

			path_of(f, false, final, sizeof(final));
			rename(path, final);
		}
	}
	closedir(d);
	/* отправлять по порядку записи */
	for (int i = 0; i < n; i++) {
		uint32_t rank = 1;

		for (int j = 0; j < n; j++) {
			rank += files[j].info.start_utc < files[i].info.start_utc;
		}
		files[i].seq = rank;
	}
	seq = n;
	printf("HBOX SD %d files to send\n", n);
}

bool store_init(void)
{
	lock = xSemaphoreCreateMutex();
	ring = xRingbufferCreateWithCaps(RING_BYTES, RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM);
	crc_buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
	if (!ring || !crc_buf) {
		return false;
	}
	xTaskCreatePinnedToCore(writer, "sd", 4096, NULL, 8, NULL, 0);
	if (!mount()) {
		return false;
	}
	scan();
	return true;
}

struct store_file *store_create(int64_t start_us, int64_t start_utc, const char *ext)
{
	struct store_file *f;

	static int64_t tried_us;

	if (failed_us && esp_timer_get_time() - failed_us < FAIL_PAUSE_US) {
		return NULL;
	}
	/* карту могли вставить после включения — пробуем раз в 30 с */
	if (!card && (tried_us && esp_timer_get_time() - tried_us < 30000000)) {
		return NULL;
	}
	tried_us = esp_timer_get_time();
	if (!mount()) {
		return NULL;
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	f = oldest(FREE);
	if (!f && drop_oldest()) {
		f = oldest(FREE);
	}
	if (f) {
		*f = (struct store_file){.state = RECORDING, .seq = ++seq};
		strncpy(f->ext, ext, sizeof(f->ext) - 1);
		f->info = (struct store_info){.start_us = start_us, .start_utc = start_utc, .ext = f->ext};
	}
	xSemaphoreGive(lock);
	if (f) {
		struct op op = {.type = OP_CREATE, .f = f};

		stage.op.f = f;
		stage_len = 0;
		if (!send_op(&op, 0)) {
			xSemaphoreTake(lock, portMAX_DELAY);
			f->state = FREE;
			xSemaphoreGive(lock);
			f = NULL;
		}
	}
	return f;
}

bool store_write(struct store_file *f, const void *data, size_t len)
{
	const uint8_t *p = data;

	if (f->failed) {
		return false;
	}
	while (len) {
		size_t n = STAGE_BYTES - stage_len < len ? STAGE_BYTES - stage_len : len;

		memcpy(stage.data + stage_len, p, n);
		stage_len += n;
		p += n;
		len -= n;
		if (stage_len == STAGE_BYTES && !flush_stage()) {
			return false;
		}
	}
	return true;
}

bool store_pwrite(struct store_file *f, uint32_t offset, const void *data, size_t len)
{
	struct {
		struct op op;
		uint8_t data[16];
	} m = {.op = {.type = OP_PWRITE, .f = f, .offset = offset}};

	if (len > sizeof(m.data) || !flush_stage()) {
		return false;
	}
	memcpy(m.data, data, len);
	return send_op(&m.op, len);
}

void store_close(struct store_file *f)
{
	struct op op = {.type = OP_CLOSE, .f = f};

	flush_stage();
	/* закрытие обязано дойти до карты — иначе файл навсегда остался бы RECORDING */
	while (xRingbufferSend(ring, &op, sizeof(op), pdMS_TO_TICKS(1000)) != pdTRUE) {
	}
}

/* --- отправка --- */

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
		name_late(f);
	}
	xSemaphoreGive(lock);
	if (f && !f->crc_known) {
		/* CRC — по карте, в задаче отправки (час mp3 — ~57 МБ, ~30 с) */
		uint32_t crc = 0, off = 0;
		size_t n;

		while ((n = store_read(f, off, crc_buf, 16384)) > 0) {
			crc = esp_rom_crc32_le(crc, crc_buf, n);
			off += n;
		}
		if (off != f->info.size) {
			printf("HBOX SD ERROR read %lu of %lu\n", (unsigned long)off, (unsigned long)f->info.size);
			return NULL;
		}
		f->info.crc = crc;
		f->crc_known = true;
	}
	if (f) {
		*info = f->info;
	}
	return f;
}

size_t store_read(struct store_file *f, uint32_t offset, void *buf, size_t len)
{
	char path[48];

	if (offset >= f->info.size) {
		return 0;
	}
	if (rd_f != f) {
		if (rd) {
			fclose(rd);
		}
		path_of(f, false, path, sizeof(path));
		rd = fopen(path, "rb");
		rd_f = rd ? f : NULL;
		if (!rd) {
			return 0;
		}
	}
	len = len < f->info.size - offset ? len : f->info.size - offset;
	if (fseek(rd, offset, SEEK_SET)) {
		return 0;
	}
	return fread(buf, 1, len, rd);
}

void store_sent(struct store_file *f)
{
	char path[48];

	if (rd_f == f) {
		fclose(rd);
		rd = NULL;
		rd_f = NULL;
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	path_of(f, false, path, sizeof(path));
	unlink(path);
	f->state = FREE;
	xSemaphoreGive(lock);
}

uint32_t store_pending(void)
{
	uint32_t n = 0;

	xSemaphoreTake(lock, portMAX_DELAY);
	for (int i = 0; i < FILES_MAX; i++) {
		n += files[i].state == READY || files[i].state == SENDING;
	}
	xSemaphoreGive(lock);
	return n;
}

uint32_t store_dropped(void)
{
	return dropped;
}
