#include "uplink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "clock.h"
#include "modem.h"
#include "recorder.h"
#include "store.h"

#define SERVER "178.104.149.56"
#define PORT 18443
#define SOCK 0
#define SSL_ID 1
#define CA_NAME "hbox_ca.pem"
#define CONFIG_EVERY_S (5 * 60)

extern const char ca_pem_start[] asm("_binary_hbox_ca_pem_start");
extern const char ca_pem_end[] asm("_binary_hbox_ca_pem_end");

static struct settings *cfg;
static struct uplink_stats stats = {.state = "modem", .csq = -1};
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static bool sock_open;
/* nginx закрывает простаивающее соединение через 120 с — заранее открываем новое */
static int64_t sock_used_us;
#define SOCK_IDLE_US (100 * 1000000LL)
/* тело запроса кусками из хранилища; ответ сервера */
static uint8_t chunk[MODEM_SEND_MAX];
static char resp[1024];

void uplink_get_stats(struct uplink_stats *st)
{
	taskENTER_CRITICAL(&mux);
	*st = stats;
	taskEXIT_CRITICAL(&mux);
}

static void set_state(const char *s)
{
	stats.state = s;
}

/* --- модем и TLS --- */

static esp_err_t modem_setup(void)
{
	char line[MODEM_LINE_MAX], cmd[96];
	esp_err_t err;

	set_state("modem");
	err = modem_open();
	if (err) {
		printf("HBOX UP modem not found (%s)\n", esp_err_to_name(err));
		return err;
	}
	set_state("net");
	for (int i = 0;; i++) {
		line[0] = '\0';
		if (modem_at("AT+CEREG?", "+CEREG:", line, 2000) == ESP_OK &&
		    (strstr(line, ",1") || strstr(line, ",5"))) {
			break;
		}
		if (i == 60) {
			printf("HBOX UP no network (%s)\n", line);
			return ESP_ERR_TIMEOUT;
		}
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
	line[0] = '\0';
	if (modem_at("AT+CSQ", "+CSQ:", line, 1000) == ESP_OK) {
		stats.csq = atoi(line + 6);
	}
	/* мобильный интернет: обычно включается сам */
	line[0] = '\0';
	modem_at("AT+MIPCALL?", "+MIPCALL:", line, 2000);
	if (!strstr(line, "+MIPCALL: 1,1")) {
		modem_at("AT+MIPCALL=1,1", NULL, NULL, 5000);
		modem_wait("+MIPCALL:", line, 30000);
	}

	/* TLS с проверкой сервера по нашему CA */
	set_state("tls");
	snprintf(cmd, sizeof(cmd), "AT+MSSLCERTWR=\"%s\",0,%u", CA_NAME,
		 (unsigned)strlen(ca_pem_start));
	err = modem_at_data(cmd, ca_pem_start, strlen(ca_pem_start), 5000);
	snprintf(cmd, sizeof(cmd), "AT+MSSLCFG=\"auth\",%d,1", SSL_ID);
	err = err ?: modem_at(cmd, NULL, NULL, 2000);
	snprintf(cmd, sizeof(cmd), "AT+MSSLCFG=\"cert\",%d,\"%s\"", SSL_ID, CA_NAME);
	err = err ?: modem_at(cmd, NULL, NULL, 2000);
	snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"ssl\",%d,1,%d", SOCK, SSL_ID);
	err = err ?: modem_at(cmd, NULL, NULL, 2000);
	if (err) {
		printf("HBOX UP tls setup failed\n");
	}
	return err;
}

static void sock_close(void)
{
	char cmd[24];

	snprintf(cmd, sizeof(cmd), "AT+MIPCLOSE=%d", SOCK);
	modem_at(cmd, NULL, NULL, 5000);
	sock_open = false;
}

static esp_err_t sock_connect(void)
{
	char cmd[80], line[MODEM_LINE_MAX] = "";

	if (sock_open && !modem_sock_closed() && esp_timer_get_time() - sock_used_us < SOCK_IDLE_US) {
		return ESP_OK;
	}
	sock_close();
	modem_sock_reset();
	snprintf(cmd, sizeof(cmd), "AT+MIPOPEN=%d,\"TCP\",\"%s\",%d,60,0", SOCK, SERVER, PORT);
	if (modem_at(cmd, NULL, line, 5000) != ESP_OK || modem_wait("+MIPOPEN:", line, 70000) != ESP_OK ||
	    !strstr(line, ",0")) {
		printf("HBOX UP connect failed %s\n", line);
		return ESP_FAIL;
	}
	sock_open = true;
	return ESP_OK;
}

/* --- HTTP поверх сокета --- */

/*
 * Запрос с телом из файла хранилища (body_len байт с позиции body_off) или без тела.
 * Ответ — в resp (тело), возвращает код HTTP или -1 при обрыве.
 */
static int http(const char *method, const char *path, struct store_file *f, uint32_t body_off,
		uint32_t body_len)
{
	char head[384];
	size_t have = 0, need;
	int code = -1, n;
	char *body;

	resp[0] = '\0';
	if (sock_connect() != ESP_OK) {
		return -1;
	}
	n = snprintf(head, sizeof(head),
		     "%s %s HTTP/1.1\r\nHost: %s\r\nAuthorization: Bearer %s\r\n"
		     "Content-Length: %u\r\nContent-Type: application/octet-stream\r\n\r\n",
		     method, path, SERVER, cfg->token, (unsigned)body_len);
	if (modem_sock_send(SOCK, head, n, 30000) != ESP_OK) {
		goto broken;
	}
	/* 8 КБ модем принимает за доли секунды; 10 с без OK — он потерял байты и ждёт остаток */
	for (uint32_t off = 0; off < body_len;) {
		size_t len = store_read(f, body_off + off, chunk,
					body_len - off < sizeof(chunk) ? body_len - off : sizeof(chunk));

		if (!len || modem_sock_send(SOCK, chunk, len, 10000) != ESP_OK) {
			goto broken;
		}
		off += len;
	}

	/* заголовки ответа, затем тело по Content-Length; сервер отвечает сразу — 15 с без ответа значит,
	 * до него дошло не всё (модем потерял кусок), ждать дальше бесполезно */
	while (!(body = strstr(resp, "\r\n\r\n"))) {
		if (have >= sizeof(resp) - 1) {
			goto broken;
		}
		n = modem_sock_recv(resp + have, sizeof(resp) - 1 - have, 15000);
		if (n <= 0) {
			goto broken;
		}
		have += n;
		resp[have] = '\0';
	}
	if (sscanf(resp, "HTTP/1.%*d %d", &code) != 1) {
		goto broken;
	}
	char *cl = strcasestr(resp, "\r\nContent-Length:");

	body += 4;
	need = (body - resp) + (cl ? strtoul(cl + 17, NULL, 10) : 0);
	while (have < need && have < sizeof(resp) - 1) {
		n = modem_sock_recv(resp + have, sizeof(resp) - 1 - have, 15000);
		if (n <= 0) {
			goto broken;
		}
		have += n;
		resp[have] = '\0';
	}
	if (strcasestr(resp, "\r\nConnection: close")) {
		sock_close();
	}
	memmove(resp, body, strlen(body) + 1);
	sock_used_us = esp_timer_get_time();
	return code;

broken:
	sock_close();
	resp[0] = '\0';
	return -1;
}

/* --- маленький разбор JSON ответов сервера --- */

static const char *json_value(const char *key)
{
	char pat[40];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\":", key);
	p = strstr(resp, pat);
	if (!p) {
		return NULL;
	}
	p += strlen(pat);
	while (*p == ' ') {
		p++;
	}
	return p;
}

static bool json_str(const char *key, char *out, size_t cap)
{
	const char *p = json_value(key), *end;

	if (!p || *p != '"' || !(end = strchr(p + 1, '"')) || (size_t)(end - p - 1) >= cap) {
		return false;
	}
	memcpy(out, p + 1, end - p - 1);
	out[end - p - 1] = '\0';
	return true;
}

static long json_long(const char *key, long def)
{
	const char *p = json_value(key);

	return p && (*p == '-' || (*p >= '0' && *p <= '9')) ? strtol(p, NULL, 10) : def;
}

static int json_bool(const char *key, int def)
{
	const char *p = json_value(key);

	return !p ? def : strncmp(p, "true", 4) == 0 ? 1 : strncmp(p, "false", 5) == 0 ? 0 : def;
}

/* --- настройки и файлы --- */

/* Настройки; заодно состояние устройства для сайта. Возвращает, через сколько секунд спросить снова. */
static int fetch_config(void)
{
	char channel[sizeof(cfg->channel)], format[8], t[32], path[256];
	struct recorder_stats rec;
	int code;

	recorder_get_stats(&rec);
	snprintf(path, sizeof(path),
		 "/hbox/v1/config?rec=%d&files=%lu&pending=%lu&dropped=%lu&overflows=%lu&rec_errors=%lu"
		 "&cpu=%lu&csq=%d&up_errors=%lu&sent=%lu&kbps=%lu&uptime=%lld",
		 rec.recording, (unsigned long)rec.files, (unsigned long)store_pending(),
		 (unsigned long)store_dropped(), (unsigned long)rec.overflows, (unsigned long)rec.errors,
		 (unsigned long)rec.cpu10 / 10, stats.csq, (unsigned long)stats.errors,
		 (unsigned long)stats.sent_files, (unsigned long)stats.kbps10 / 10,
		 esp_timer_get_time() / 60000000);
	code = http("GET", path, NULL, 0, 0);

	if (code != 200) {
		printf("HBOX UP config failed (%d) %s\n", code, code == 401 ? "- wrong token" : "");
		return 0;
	}
	/* время — с сервера при каждом запросе: AT+CCLK у ML307R отдаёт UTC с поясом, а по
	 * документации должно быть местное, — такому не доверяем */
	if (json_str("time", t, sizeof(t))) {
		bool first = !clock_valid();

		if (clock_set_iso(t) && first) {
			printf("HBOX UP time from server %s\n", t);
		}
	}
	struct settings s = *cfg;

	if (json_str("channel", channel, sizeof(channel))) {
		strcpy(s.channel, channel);
	}
	if (json_str("format", format, sizeof(format)) && strlen(format) < sizeof(s.format)) {
		strcpy(s.format, format);
	}
	s.bitrate = json_long("bitrate", s.bitrate);
	s.record = json_bool("record", s.record);
	if (memcmp(&s, cfg, sizeof(s)) != 0) {
		*cfg = s;
		settings_save(cfg);
		store_set_channel(cfg->channel);
		printf("HBOX UP config channel=%s format=%s bitrate=%d record=%d\n", s.channel, s.format,
		       s.bitrate, s.record);
	}
	recorder_configure(cfg->format, cfg->bitrate, cfg->record);
	/* сервер знает расписание и говорит, когда спросить снова (к смене пауза/запись) */
	long recheck = json_long("recheck", CONFIG_EVERY_S);

	return recheck < 5 ? 5 : recheck > CONFIG_EVERY_S ? CONFIG_EVERY_S : recheck;
}

/* <канал>_YYYYMMDD_HHMMSS.<ext>; false — времени ещё нет */
static bool file_name(const struct store_info *info, char *out, size_t cap)
{
	time_t t = info->start_utc ? info->start_utc : clock_at(info->start_us);
	struct tm tm;

	if (!t || !cfg->channel[0]) {
		return false;
	}
	gmtime_r(&t, &tm);
	snprintf(out, cap, "%s_%04d%02d%02d_%02d%02d%02d.%s", cfg->channel, tm.tm_year + 1900,
		 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, info->ext);
	return true;
}


/* Тело одного PUT: ~30 с отправки. Кусок, испорченный модемом, сервер выбрасывает целиком (upto). */
#define PART_BYTES (1024 * 1024)

static int64_t config_at;

/* Настройки, если пора, — и между кусками часового файла. 1 — спросили, 0 — не пора, -1 — ошибка. */
static int config_if_due(void)
{
	if (esp_timer_get_time() < config_at) {
		return 0;
	}
	set_state("idle");
	int recheck = fetch_config();

	if (!recheck) {
		return -1;
	}
	config_at = esp_timer_get_time() + recheck * 1000000LL;
	return 1;
}

/* *crc дополняется CRC-32 байт файла [off, off + len). */
static bool crc_range(struct store_file *f, uint32_t *crc, uint32_t off, uint32_t len)
{
	while (len) {
		size_t n = store_read(f, off, chunk, len < sizeof(chunk) ? len : sizeof(chunk));

		if (!n) {
			return false;
		}
		*crc = esp_rom_crc32_le(*crc, chunk, n);
		off += n;
		len -= n;
	}
	return true;
}

/*
 * Сколько из принятого сервером совпадает с файлом: сервер присылает CRC начала файла в нескольких
 * точках ("check": "смещение:crc,..."), берём последнюю совпавшую (в *crc_at — CRC до неё).
 * Модем изредка теряет байты — хвост на сервере тогда испорчен.
 */
static long verified_size(struct store_file *f, long size, uint32_t *crc_at)
{
	char check[320], *p = check, *end;
	uint32_t crc = 0, pos = 0;
	long good = 0;

	*crc_at = 0;
	if (size <= 0) {
		return 0;
	}
	if (!json_str("check", check, sizeof(check))) {
		return crc_range(f, crc_at, 0, size) ? size : 0;
	}
	while (*p) {
		unsigned long off = strtoul(p, &end, 10);

		if (*end != ':' || off > (unsigned long)size) {
			break;
		}
		uint32_t want = strtoul(end + 1, &end, 16);

		if (!crc_range(f, &crc, pos, off - pos)) {
			break;
		}
		pos = off;
		if (crc == want) {
			good = off;
			*crc_at = crc;
		}
		p = *end == ',' ? end + 1 : end;
	}
	return good;
}

/* Отправляет файл кусками по PART_BYTES до конца или до первой ошибки. true — сервер принял файл целиком. */
static bool send_file(struct store_file *f, const struct store_info *info)
{
	char name[96], path[256];
	int64_t t0 = esp_timer_get_time();
	uint32_t crc;
	long size, start;
	int code;

	file_name(info, name, sizeof(name));
	snprintf(path, sizeof(path), "/hbox/v1/files/%s", name);
	code = http("GET", path, NULL, 0, 0);
	if (code != 200) {
		printf("HBOX UP status %s failed (%d)\n", name, code);
		return false;
	}
	if (json_bool("done", 0) == 1) {
		return true;
	}
	size = json_long("size", 0);
	if (size < 0 || size > (long)info->size) {
		size = 0;
	}
	start = verified_size(f, size, &crc);
	if (start != size) {
		printf("HBOX UP %s: server has %ld, matches up to %ld\n", name, size, start);
	}
	for (size = start;;) {
		uint32_t len = info->size - size < PART_BYTES ? info->size - size : PART_BYTES;
		uint32_t upto = crc;

		if (!crc_range(f, &upto, size, len)) {
			printf("HBOX UP %s: read failed at %ld\n", name, size);
			return false;
		}
		set_state("send");
		snprintf(path, sizeof(path), "/hbox/v1/files/%s?offset=%ld&total=%lu&crc=%08lx&upto=%08lx",
			 name, size, (unsigned long)info->size, (unsigned long)info->crc, (unsigned long)upto);
		code = http("PUT", path, f, size, len);
		/* 422 — кусок испорчен по дороге и выброшен сервером; обрыв, 409 — снова со статуса */
		if (code != 200) {
			printf("HBOX UP send %s failed (%d) at %ld %s\n", name, code, size, resp);
			return false;
		}
		if (json_bool("done", 0) == 1) {
			break;
		}
		if (json_long("size", -1) != size + (long)len) {
			printf("HBOX UP %s: server size mismatch %s\n", name, resp);
			return false;
		}
		size += len;
		crc = upto;
		if (config_if_due() < 0) {
			return false;
		}
	}
	double sec = (esp_timer_get_time() - t0) / 1e6;

	taskENTER_CRITICAL(&mux);
	stats.sent_files++;
	stats.sent_kb += (info->size - start) / 1024;
	stats.kbps10 = sec > 0 ? (info->size - start) / 102.4 / sec : 0;
	taskEXIT_CRITICAL(&mux);
	printf("HBOX UP sent %s %lu B (from %ld) in %.1f s = %.1f KB/s\n", name, (unsigned long)info->size,
	       start, sec, (info->size - start) / 1024.0 / sec);
	return true;
}

static void task(void *arg)
{
	int failures = 0;
	bool ready = false;

	for (;;) {
		if (!cfg->token[0]) {
			set_state("token");
			printf("HBOX UP no token: type t <token> in the console\n");
			vTaskDelay(pdMS_TO_TICKS(30000));
			continue;
		}
		if (!ready || failures >= 5) {
			sock_open = false;
			ready = modem_setup() == ESP_OK;
			failures = 0;
			if (!ready) {
				vTaskDelay(pdMS_TO_TICKS(10000));
				continue;
			}
			config_at = 0;
		}
		int got = config_if_due();

		if (got < 0) {
			failures++;
			stats.errors++;
			vTaskDelay(pdMS_TO_TICKS(5000));
			continue;
		}
		if (got > 0) {
			failures = 0;
		}

		struct store_info info;
		struct store_file *f = store_next(&info);

		char name[96];

		if (!f || !file_name(&info, name, sizeof(name))) {
			/* нечего слать или ещё нет времени/канала для имени файла */
			set_state("idle");
			vTaskDelay(pdMS_TO_TICKS(2000));
			continue;
		}
		if (send_file(f, &info)) {
			store_sent(f);
			failures = 0;
		} else {
			set_state("wait");
			failures++;
			stats.errors++;
			vTaskDelay(pdMS_TO_TICKS(5000));
		}
	}
}

void uplink_start(struct settings *s)
{
	cfg = s;
	xTaskCreatePinnedToCore(task, "uplink", 8192, NULL, 5, NULL, 0);
}
