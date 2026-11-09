#include "uplink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#define CONFIG_EVERY_US (5 * 60 * 1000000LL)

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
	line[0] = '\0';
	if (!clock_valid() && modem_at("AT+CCLK?", "+CCLK:", line, 1000) == ESP_OK &&
	    clock_set_cclk(line)) {
		printf("HBOX UP time from network %s\n", line);
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
	for (uint32_t off = 0; off < body_len;) {
		size_t len = store_read(f, body_off + off, chunk,
					body_len - off < sizeof(chunk) ? body_len - off : sizeof(chunk));

		if (!len || modem_sock_send(SOCK, chunk, len, 30000) != ESP_OK) {
			goto broken;
		}
		off += len;
	}

	/* заголовки ответа, затем тело по Content-Length */
	while (!(body = strstr(resp, "\r\n\r\n"))) {
		if (have >= sizeof(resp) - 1) {
			goto broken;
		}
		n = modem_sock_recv(resp + have, sizeof(resp) - 1 - have, 60000);
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
		n = modem_sock_recv(resp + have, sizeof(resp) - 1 - have, 30000);
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

static bool fetch_config(void)
{
	char channel[sizeof(cfg->channel)], format[8], t[32];
	int code = http("GET", "/hbox/v1/config", NULL, 0, 0);

	if (code != 200) {
		printf("HBOX UP config failed (%d) %s\n", code, code == 401 ? "- wrong token" : "");
		return false;
	}
	if (!clock_valid() && json_str("time", t, sizeof(t)) && clock_set_iso(t)) {
		printf("HBOX UP time from server %s\n", t);
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
		printf("HBOX UP config channel=%s format=%s bitrate=%d record=%d\n", s.channel, s.format,
		       s.bitrate, s.record);
	}
	recorder_configure(cfg->format, cfg->bitrate, cfg->record);
	return true;
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

/* Отправляет файл до конца или до обрыва. true — сервер подтвердил файл целиком. */
static bool send_file(struct store_file *f, const struct store_info *info)
{
	char name[96], path[224];
	int code;
	long size;

	file_name(info, name, sizeof(name));
	snprintf(path, sizeof(path), "/hbox/v1/files/%s", name);
	code = http("GET", path, NULL, 0, 0);
	if (code != 200) {
		printf("HBOX UP status %s failed (%d)\n", name, code);
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		if (json_bool("done", 0) == 1) {
			return true;
		}
		size = json_long("size", 0);
		if (size < 0 || size > (long)info->size) {
			size = 0;
		}
		int64_t t0 = esp_timer_get_time();

		set_state("send");
		snprintf(path, sizeof(path), "/hbox/v1/files/%s?offset=%ld&total=%lu&crc=%08lx", name, size,
			 (unsigned long)info->size, (unsigned long)info->crc);
		code = http("PUT", path, f, size, info->size - size);
		double sec = (esp_timer_get_time() - t0) / 1e6;

		if (code == 200 && json_bool("done", 0) == 1) {
			taskENTER_CRITICAL(&mux);
			stats.sent_files++;
			stats.sent_kb += (info->size - size) / 1024;
			stats.kbps10 = sec > 0 ? (info->size - size) / 102.4 / sec : 0;
			taskEXIT_CRITICAL(&mux);
			printf("HBOX UP sent %s %lu B (from %ld) in %.1f s = %.1f KB/s\n", name,
			       (unsigned long)info->size, size, sec, (info->size - size) / 1024.0 / sec);
			return true;
		}
		/* 409 — сервер сказал, сколько у него есть; 422 — CRC не сошёлся, заново с 0 */
		if (code != 409 && code != 422) {
			printf("HBOX UP send %s failed (%d) %s\n", name, code, resp);
			return false;
		}
		printf("HBOX UP %s: %d, retry %s\n", name, code, resp);
	}
	return false;
}

static void task(void *arg)
{
	int64_t config_at = 0;
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
		if (esp_timer_get_time() >= config_at) {
			set_state("idle");
			if (fetch_config()) {
				config_at = esp_timer_get_time() + CONFIG_EVERY_US;
				failures = 0;
			} else {
				failures++;
				stats.errors++;
				vTaskDelay(pdMS_TO_TICKS(5000));
				continue;
			}
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
