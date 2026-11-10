/*
 * Хранилище на microSD (модуль YS-41 по SPI): часовые файлы лежат на карте до подтверждения сервером,
 * потом переезжают в sent/ и хранятся там 7 дней (ТЗ п. 4). Места не хватает — сначала удаляются
 * самые старые отправленные, и только потом неотправленные.
 *
 * Запись карты бывает медленной (до сотен мс на кадр FAT), а у микрофона буфер ~0,5 с — поэтому
 * recorder кладёт данные в кольцо в PSRAM, а на карту их пишет отдельная задача.
 *
 * Имена на карте — как на сервере: <канал>_YYYYMMDD_HHMMSS.<ext> (начало по UTC); пока пишется — с .part;
 * если время начала ещё неизвестно — u<номер>.<ext>, переименовывается, как только время придёт.
 * Старые имена <секунды Unix>.<ext> при включении переименовываются в новые. После перезагрузки .part от mp3
 * отправляется как есть (кадры MP3 независимы), m4a без moov — пока выбрасывается (hbox-5nq).
 */
#include "store.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
#define SENT_DIR SD_DIR "/sent"
#define SENT_KEEP_S (7 * 86400)
#define FILES_MAX 256
#define NAME_MAX_LEN 128
#define PATH_LEN (NAME_MAX_LEN + 32)
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
	char chan[33];
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
static char channel[33] = "hbox";
/* последняя ошибка карты: после неё новый файл — не раньше чем через FAIL_PAUSE_US */
static volatile int64_t failed_us;
#define FAIL_PAUSE_US 10000000LL
/* FAT пишет размер файла в каталог только при fsync/закрытии: без этого при выключении пропал бы час */
#define SYNC_EVERY_US 10000000LL
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

/* <канал>_YYYYMMDD_HHMMSS.<ext> — как на сервере */
static void name_of(const struct store_file *f, char *out, size_t cap)
{
	time_t t = f->info.start_utc;
	struct tm tm;

	gmtime_r(&t, &tm);
	snprintf(out, cap, "%s_%04d%02d%02d_%02d%02d%02d.%s", f->chan, tm.tm_year + 1900, tm.tm_mon + 1,
		 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, f->ext);
}

static void path_of(const struct store_file *f, bool part, char *out, size_t cap)
{
	if (f->info.start_utc) {
		char name[NAME_MAX_LEN];

		name_of(f, name, sizeof(name));
		snprintf(out, cap, SD_DIR "/%s%s", name, part ? ".part" : "");
	} else {
		snprintf(out, cap, SD_DIR "/u%lu.%s%s", (unsigned long)f->seq, f->ext, part ? ".part" : "");
	}
}

/*
 * Время начала из имени: <канал>_YYYYMMDD_HHMMSS.… или старое <секунды Unix>.…
 * В chan — канал (для старых имён — текущий), в *rest — всё после первой точки. 0 — не наше имя.
 */
static int64_t parse_name(const char *name, char *chan, size_t cap, const char **rest)
{
	/* точка перед расширением, а не в имени канала */
	const char *dot = strstr(name, ".mp3");

	if (!dot) {
		dot = strstr(name, ".m4a");
	}
	size_t n = dot ? (size_t)(dot - name) : 0;
	int y, mo, d, h, mi, s;
	size_t i;

	if (!n) {
		return 0;
	}
	*rest = dot + 1;
	for (i = 0; i < n && isdigit((unsigned char)name[i]); i++) {
	}
	if (i == n) {
		snprintf(chan, cap, "%s", channel);
		return strtoll(name, NULL, 10);
	}
	if (n < 17 || n - 16 >= cap || name[n - 16] != '_' || name[n - 7] != '_' ||
	    sscanf(name + n - 15, "%4d%2d%2d_%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
		return 0;
	}
	memcpy(chan, name, n - 16);
	chan[n - 16] = '\0';
	return clock_utc(y, mo, d, h, mi, s);
}

void store_set_channel(const char *ch)
{
	if (!ch[0] || strlen(ch) >= sizeof(channel)) {
		return;
	}
	if (lock) {
		xSemaphoreTake(lock, portMAX_DELAY);
	}
	strcpy(channel, ch);
	if (lock) {
		xSemaphoreGive(lock);
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
	mkdir(SENT_DIR, 0775);
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
	char path[PATH_LEN];

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

/* Самый старый файл в sent/: имя в out; false — пусто. */
static bool oldest_sent(long long *utc, char *out, size_t cap)
{
	DIR *d = opendir(SENT_DIR);
	struct dirent *e;
	bool found = false;

	if (!d) {
		return false;
	}
	while ((e = readdir(d))) {
		char chan[33];
		const char *rest;
		long long t = parse_name(e->d_name, chan, sizeof(chan), &rest);

		if (t > 0 && (!found || t < *utc)) {
			*utc = t;
			snprintf(out, cap, SENT_DIR "/%s", e->d_name);
			found = true;
		}
	}
	closedir(d);
	return found;
}

/* Удалить отправленные старше 7 дней, а при нехватке места — самые старые из них. */
static void prune_sent(void)
{
	int64_t now = clock_now();
	long long utc;
	char path[300];

	while (oldest_sent(&utc, path, sizeof(path))) {
		bool old = now && utc < now - SENT_KEEP_S;

		if (!old && free_bytes() >= KEEP_FREE) {
			break;
		}
		if (unlink(path)) {
			printf("HBOX SD ERROR remove %s\n", path);
			break;
		}
		printf("HBOX SD removed %s (%s)\n", path, old ? "7 days" : "space");
	}
}

/* Время начала пришло позже, чем файл начался: переименовать u<номер> в <UTC>. */
static void name_late(struct store_file *f)
{
	char from[PATH_LEN], to[PATH_LEN];
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
	char path[PATH_LEN], final[PATH_LEN];
	int64_t synced_us = 0;

	for (;;) {
		size_t len;
		struct op *op = xRingbufferReceive(ring, &len, portMAX_DELAY);
		struct store_file *f = op->f;
		size_t n = len - sizeof(*op);

		switch (op->type) {
		case OP_CREATE:
			prune_sent();
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
				if (esp_timer_get_time() - synced_us >= SYNC_EVERY_US) {
					fflush(out);
					fsync(fileno(out));
					synced_us = esp_timer_get_time();
				}
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
		char path[300], ext[8] = "", part[8] = "", chan[33] = "";
		const char *rest = "";
		long long utc;
		struct stat st;

		if (strcmp(e->d_name, "sent") == 0) {
			continue;
		}
		snprintf(path, sizeof(path), SD_DIR "/%s", e->d_name);
		utc = parse_name(e->d_name, chan, sizeof(chan), &rest);
		if (sscanf(rest, "%3[a-z0-9].%5s", ext, part) < 1 || utc <= 0 ||
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
		strcpy(f->chan, chan);
		f->info = (struct store_info){.start_utc = utc, .ext = f->ext, .size = st.st_size};
		/* .part — mp3 оборвался при выключении, отправляем что успело записаться; старое имя — в новое */
		char final[PATH_LEN];

		path_of(f, false, final, sizeof(final));
		if (strcmp(path, final) && rename(path, final)) {
			printf("HBOX SD ERROR rename %s\n", e->d_name);
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
		strcpy(f->chan, channel);
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
	char path[PATH_LEN];

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
	char path[PATH_LEN], name[NAME_MAX_LEN], to[PATH_LEN];

	if (rd_f == f) {
		fclose(rd);
		rd = NULL;
		rd_f = NULL;
	}
	xSemaphoreTake(lock, portMAX_DELAY);
	path_of(f, false, path, sizeof(path));
	name_of(f, name, sizeof(name));
	snprintf(to, sizeof(to), SENT_DIR "/%s", name);
	if (rename(path, to)) {
		printf("HBOX SD ERROR keep %s\n", path);
		unlink(path);
	}
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
