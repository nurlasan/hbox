#include "uplink.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "clock.h"
#include "modem.h"
#include "recorder.h"
#include "store.h"

#define SOCK 0
#define CA_NAME "hbox_ca.pem"
#define CF_NAME "cf_roots.pem"
#define CONFIG_EVERY_S (5 * 60)

extern const char ca_pem_start[] asm("_binary_hbox_ca_pem_start");
extern const char ca_pem_end[] asm("_binary_hbox_ca_pem_end");
extern const char cf_pem_start[] asm("_binary_cf_roots_pem_start");

/* Пути до сервера: напрямую (наш CA) и запасной через Cloudflare — обычный HTTPS на 443 с именем,
 * его не режут операторы, которые режут TLS на нестандартный порт по IP (Kcell с 12.11.2026). */
static const struct route {
	const char *host;
	int port;
	int ssl_id;
	const char *name;
	const char *api; /* чем заменить "/hbox/v1" в пути: тоннель Cloudflare пропускает только /hbox/admin* */
} routes[] = {
	{"178.104.149.56", 18443, 1, "direct", "/hbox/v1"},
	{"hbox.fabtech.uz", 443, 2, "cf", "/hbox/admin/dev/v1"},
};
#define ROUTE_FAILS 3 /* столько неудачных соединений подряд — пробуем другой путь */
static int route, route_fails;
static const char *fw_version(void);
static char route_want[8] = ""; /* сервер может закрепить путь: "direct" / "cf" */

static struct settings *cfg;
static struct uplink_stats stats = {.state = "modem", .csq = -1};
static void url_safe(char *s);
/* оператор сети (AT+COPS?), для сайта */
static char op[25] = "";
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
	/* +COPS: 0,0,"KCELL",7 — имя оператора буквами */
	line[0] = '\0';
	modem_at("AT+COPS=3,0", NULL, NULL, 1000);
	if (modem_at("AT+COPS?", "+COPS:", line, 2000) == ESP_OK) {
		char *a = strchr(line, '"'), *b = a ? strchr(a + 1, '"') : NULL;

		if (b) {
			*b = '\0';
			strlcpy(op, a + 1, sizeof(op));
			url_safe(op);
			printf("HBOX UP operator %s\n", op);
		}
	}
	/* мобильный интернет: обычно включается сам */
	line[0] = '\0';
	modem_at("AT+MIPCALL?", "+MIPCALL:", line, 2000);
	if (!strstr(line, "+MIPCALL: 1,1")) {
		modem_at("AT+MIPCALL=1,1", NULL, NULL, 5000);
		modem_wait("+MIPCALL:", line, 30000);
	}

	/* TLS с проверкой сервера: напрямую — по нашему CA, через Cloudflare — по публичным корням */
	set_state("tls");
	err = ESP_OK;
	for (int i = 0; i < 2; i++) {
		const char *name = i ? CF_NAME : CA_NAME, *pem = i ? cf_pem_start : ca_pem_start;
		int id = routes[i].ssl_id;

		snprintf(cmd, sizeof(cmd), "AT+MSSLCERTWR=\"%s\",0,%u", name, (unsigned)strlen(pem));
		err = err ?: modem_at_data(cmd, pem, strlen(pem), 5000);
		snprintf(cmd, sizeof(cmd), "AT+MSSLCFG=\"auth\",%d,1", id);
		err = err ?: modem_at(cmd, NULL, NULL, 2000);
		snprintf(cmd, sizeof(cmd), "AT+MSSLCFG=\"cert\",%d,\"%s\"", id, name);
		err = err ?: modem_at(cmd, NULL, NULL, 2000);
	}
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
	for (int i = 0; i < 2; i++) {
		if (route_want[0] && !strcmp(route_want, routes[i].name)) {
			route = i;
		}
	}
	const struct route *r = &routes[route];

	snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"ssl\",%d,1,%d", SOCK, r->ssl_id);
	modem_at(cmd, NULL, NULL, 2000);
	/* режим буфера (,2): принятое ждёт в модеме, забираем AT+MIPRD — см. modem.c */
	snprintf(cmd, sizeof(cmd), "AT+MIPOPEN=%d,\"TCP\",\"%s\",%d,60,2", SOCK, r->host, r->port);
	if (modem_at(cmd, NULL, line, 5000) != ESP_OK || modem_wait("+MIPOPEN:", line, 70000) != ESP_OK ||
	    !strstr(line, ",0")) {
		printf("HBOX UP connect failed (%s) %s\n", r->name, line);
		if (++route_fails >= ROUTE_FAILS && !route_want[0]) {
			route = !route;
			route_fails = 0;
			printf("HBOX UP route -> %s\n", routes[route].name);
		}
		return ESP_FAIL;
	}
	route_fails = 0;
	sock_open = true;
	return ESP_OK;
}

/* --- HTTP поверх сокета --- */

/* Приёмник тела ответа по мере прихода (прошивка — сразу во флеш); false — прервать. */
typedef bool (*http_sink)(const uint8_t *data, size_t len, void *ctx);

/*
 * Запрос с телом из файла хранилища (body_len байт с позиции body_off) или без тела.
 * Ответ — в resp (тело), а если задан sink — тело любой длины потоком в sink.
 * Возвращает код HTTP или -1 при обрыве (принятое до обрыва sink уже получил).
 */
/* esp_timer отправки последнего запроса и прихода заголовков ответа — для часов;
 * contact_us — последний ответ сервера — для перезапуска модема */
static int64_t req_us, ans_us, contact_us;

static int http_ex(const char *method, const char *path, struct store_file *f, uint32_t body_off,
		   uint32_t body_len, http_sink sink, void *ctx)
{
	char head[768];
	size_t have = 0, need;
	int code = -1, n;
	char *body;

	resp[0] = '\0';
	if (sock_connect() != ESP_OK) {
		return -1;
	}
	n = snprintf(head, sizeof(head),
		     "%s %s%s HTTP/1.1\r\nHost: %s\r\nUser-Agent: hbox/%s\r\nAuthorization: Bearer %s\r\n"
		     "Content-Length: %u\r\nContent-Type: application/octet-stream\r\n\r\n",
		     method, strncmp(path, "/hbox/v1", 8) ? "" : routes[route].api,
		     strncmp(path, "/hbox/v1", 8) ? path : path + 8, routes[route].host, fw_version(), cfg->token,
		     (unsigned)body_len);
	if (n >= (int)sizeof(head)) {
		return -1;
	}
	req_us = esp_timer_get_time();
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
	ans_us = esp_timer_get_time();
	if (sscanf(resp, "HTTP/1.%*d %d", &code) != 1) {
		goto broken;
	}
	contact_us = ans_us;
	char *cl = strcasestr(resp, "\r\nContent-Length:");

	body += 4;
	if (sink) {
		/* тело потоком: что пришло вместе с заголовками, и остальное прямо из сокета */
		size_t clen = cl ? strtoul(cl + 17, NULL, 10) : 0, got = have - (body - resp);

		if (got > clen || (code == 200 && got && !sink((uint8_t *)body, got, ctx))) {
			goto broken;
		}
		while (got < clen) {
			n = modem_sock_recv(chunk, clen - got < sizeof(chunk) ? clen - got : sizeof(chunk), 15000);
			if (n <= 0 || (code == 200 && !sink(chunk, n, ctx))) {
				goto broken;
			}
			got += n;
		}
		if (strcasestr(resp, "\r\nConnection: close")) {
			sock_close();
		}
		resp[0] = '\0';
		sock_used_us = esp_timer_get_time();
		return code;
	}
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

static int http(const char *method, const char *path, struct store_file *f, uint32_t body_off,
		uint32_t body_len)
{
	return http_ex(method, path, f, body_off, body_len, NULL, NULL);
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

/* --- обновление прошивки по воздуху (docs/protocol.md) --- */

/* ответ с куском (+ заголовки) обязан влезть в приёмный буфер модема SOCK_RX (16 КБ) целиком */
/* не получилось — следующая попытка не раньше чем через столько */
#define OTA_RETRY_US (10 * 60 * 1000000LL)

static struct {
	char name[24], version[33];
	uint32_t size, crc;
	bool due;
} ota;
static int64_t ota_retry_at;
/* версия, которая не запустилась и откатилась загрузчиком, — её не ставим снова */
static char fw_bad[33];

/* для строки запроса: только [A-Za-z0-9._-] */
static void url_safe(char *s)
{
	for (; *s; s++) {
		if (!isalnum((unsigned char)*s) && !strchr("._-", *s)) {
			*s = '_';
		}
	}
}

static const char *fw_version(void)
{
	static char v[33];

	if (!v[0]) {
		strlcpy(v, esp_app_get_description()->version, sizeof(v));
		url_safe(v);
	}
	return v;
}

static void ota_init(void)
{
	const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
	esp_app_desc_t d;

	if (bad && esp_ota_get_partition_description(bad, &d) == ESP_OK) {
		strlcpy(fw_bad, d.version, sizeof(fw_bad));
		url_safe(fw_bad);
	}
	printf("HBOX UP firmware %s%s%s\n", fw_version(), fw_bad[0] ? ", rolled back from " : "", fw_bad);
}

/* Сервер ответил — новая прошивка работает, откат больше не нужен. */
static void ota_mark_valid(void)
{
	static bool done;
	esp_ota_img_states_t st;

	if (done) {
		return;
	}
	done = true;
	if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
	    st == ESP_OTA_IMG_PENDING_VERIFY) {
		esp_ota_mark_app_valid_cancel_rollback();
		printf("HBOX UP firmware %s confirmed\n", fw_version());
	}
}

/* Из ответа на /config: есть ли прошивка новее нашей. */
static void ota_check(void)
{
	char crc[12];

	ota.due = false;
	if (!json_str("ota_version", ota.version, sizeof(ota.version)) ||
	    !json_str("ota_name", ota.name, sizeof(ota.name)) || !json_str("ota_crc", crc, sizeof(crc))) {
		return;
	}
	url_safe(ota.version);
	ota.size = json_long("ota_size", 0);
	ota.crc = strtoul(crc, NULL, 16);
	ota.due = ota.size && strcmp(ota.version, fw_version()) && strcmp(ota.version, fw_bad) &&
		  esp_timer_get_time() >= ota_retry_at;
}

/* блок прошивки с своей CRC (сервер, ?blocks=1): испорченный модемом блок не попадает во флеш */
#define OTA_BLOCK 32768
#define OTA_BLOCKS_MAX 64

struct ota_dl {
	esp_ota_handle_t h;
	uint32_t off, crc; /* записано во флеш (по границе блока) и CRC записанного */
	uint32_t fill;     /* принято в buf текущего блока */
	uint8_t *buf;
	uint32_t blocks[OTA_BLOCKS_MAX];
};

static bool ota_sink(const uint8_t *data, size_t len, void *ctx)
{
	struct ota_dl *d = ctx;

	while (len) {
		uint32_t block = ota.size - d->off < OTA_BLOCK ? ota.size - d->off : OTA_BLOCK;
		uint32_t n = block - d->fill < len ? block - d->fill : len;

		if (!block) {
			return false; /* лишние байты сверх образа */
		}
		memcpy(d->buf + d->fill, data, n);
		d->fill += n;
		data += n;
		len -= n;
		if (d->fill < block) {
			continue;
		}
		uint32_t want = d->blocks[d->off / OTA_BLOCK];

		d->fill = 0;
		if (esp_rom_crc32_le(0, d->buf, block) != want) {
			printf("HBOX UP ota block at %lu damaged\n", (unsigned long)d->off);
			return false;
		}
		if (esp_ota_write(d->h, d->buf, block) != ESP_OK) {
			return false;
		}
		d->crc = esp_rom_crc32_le(d->crc, d->buf, block);
		d->off += block;
		if (d->off % (128 * 1024) == 0) {
			printf("HBOX UP ota %lu/%lu KB\n", (unsigned long)d->off / 1024, (unsigned long)ota.size / 1024);
		}
	}
	return true;
}

/* CRC блоков с сервера; false — не получили */
static bool ota_blocks(struct ota_dl *d)
{
	char path[96];
	const char *p = resp;
	uint32_t n = (ota.size + OTA_BLOCK - 1) / OTA_BLOCK;

	if (n > OTA_BLOCKS_MAX) {
		return false;
	}
	snprintf(path, sizeof(path), "/hbox/v1/firmware/%s?blocks=1", ota.name);
	if (http("GET", path, NULL, 0, 0) != 200) {
		return false;
	}
	for (uint32_t i = 0; i < n; i++) {
		char *end;

		d->blocks[i] = strtoul(p, &end, 16);
		if (end == p || (i + 1 < n && *end != ',')) {
			return false;
		}
		p = end + 1;
	}
	return true;
}

/*
 * Скачать одним запросом (испорченный блок или обрыв — остаток с начала блока), проверить и
 * перезагрузиться в новую прошивку. Возврат — только при ошибке.
 */
static void ota_run(void)
{
	const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
	static struct ota_dl d;
	int64_t t0 = esp_timer_get_time();
	char path[96];
	int stuck = 0;

	ota.due = false;
	ota_retry_at = esp_timer_get_time() + OTA_RETRY_US;
	if (!part || ota.size > part->size) {
		printf("HBOX UP ota %s: no room (%lu B)\n", ota.version, (unsigned long)ota.size);
		return;
	}
	memset(&d, 0, sizeof(d));
	if (!ota_blocks(&d)) {
		printf("HBOX UP ota %s: no block list\n", ota.version);
		return;
	}
	d.buf = heap_caps_malloc(OTA_BLOCK, MALLOC_CAP_SPIRAM);
	d.buf = d.buf ? d.buf : malloc(OTA_BLOCK);
	if (!d.buf || esp_ota_begin(part, ota.size, &d.h) != ESP_OK) {
		printf("HBOX UP ota %s: begin failed\n", ota.version);
		free(d.buf);
		return;
	}
	set_state("ota");
	printf("HBOX UP ota %s -> %s, %lu B\n", fw_version(), ota.version, (unsigned long)ota.size);
	while (d.off < ota.size) {
		uint32_t from = d.off;
		int code;

		/* 10 раз подряд ни блока — связь не та, попробуем позже */
		if (stuck == 10) {
			printf("HBOX UP ota gave up at %lu\n", (unsigned long)d.off);
			goto fail;
		}
		d.fill = 0;
		snprintf(path, sizeof(path), "/hbox/v1/firmware/%s?offset=%lu&len=%lu", ota.name,
			 (unsigned long)d.off, (unsigned long)(ota.size - d.off));
		code = http_ex("GET", path, NULL, 0, 0, ota_sink, &d);
		stuck = d.off > from ? 0 : stuck + 1;
		if (d.off < ota.size) {
			printf("HBOX UP ota broke (%d) at %lu (+%lu)\n", code, (unsigned long)d.off,
			       (unsigned long)(d.off - from));
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}
	if (d.crc != ota.crc) {
		printf("HBOX UP ota crc %08lx != %08lx\n", (unsigned long)d.crc, (unsigned long)ota.crc);
		goto fail;
	}
	free(d.buf);
	/* esp_ota_end ещё раз проверяет образ (контрольная сумма и SHA-256 внутри) */
	if (esp_ota_end(d.h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
		printf("HBOX UP ota image rejected\n");
		return;
	}
	printf("HBOX UP ota done in %.0f s, restarting into %s\n", (esp_timer_get_time() - t0) / 1e6,
	       ota.version);
	vTaskDelay(pdMS_TO_TICKS(500));
	esp_restart();

fail:
	esp_ota_abort(d.h);
	free(d.buf);
}

/* --- настройки и файлы --- */

/* Настройки; заодно состояние устройства для сайта. Возвращает, через сколько секунд спросить снова. */
static int fetch_config(void)
{
	char channel[sizeof(cfg->channel)], format[8], t[32], path[512];
	struct recorder_stats rec;
	uint32_t sd_free, sd_total, pending_mb;
	int code;

	recorder_get_stats(&rec);
	store_space(&sd_free, &sd_total, &pending_mb);
	snprintf(path, sizeof(path),
		 "/hbox/v1/config?rec=%d&files=%lu&pending=%lu&dropped=%lu&overflows=%lu&rec_errors=%lu"
		 "&cpu=%lu&csq=%d&up_errors=%lu&sent=%lu&kbps=%lu&uptime=%lld&reset=%d"
		 "&sd_free=%lu&sd_total=%lu&pending_mb=%lu&fw=%s&fw_bad=%s&op=%s",
		 rec.recording, (unsigned long)rec.files, (unsigned long)store_pending(),
		 (unsigned long)store_dropped(), (unsigned long)rec.overflows, (unsigned long)rec.errors,
		 (unsigned long)rec.cpu10 / 10, stats.csq, (unsigned long)stats.errors,
		 (unsigned long)stats.sent_files, (unsigned long)stats.kbps10 / 10,
		 esp_timer_get_time() / 60000000, (int)esp_reset_reason(), (unsigned long)sd_free,
		 (unsigned long)sd_total, (unsigned long)pending_mb, fw_version(), fw_bad, op);
	code = http("GET", path, NULL, 0, 0);

	if (code != 200) {
		printf("HBOX UP config failed (%d) %s\n", code, code == 401 ? "- wrong token" : "");
		return 0;
	}
	ota_mark_valid();
	ota_check();
	/* время — с сервера при каждом запросе: AT+CCLK у ML307R отдаёт UTC с поясом, а по
	 * документации должно быть местное, — такому не доверяем */
	if (json_str("time", t, sizeof(t))) {
		bool first = !clock_valid();

		if (clock_set_iso(t, req_us, ans_us) && first) {
			printf("HBOX UP time from server %s\n", t);
		}
	}
	struct settings s = *cfg;

	char want[sizeof(route_want)] = "";

	json_str("route", want, sizeof(want));
	if (strcmp(want, route_want)) {
		strcpy(route_want, want);
		printf("HBOX UP route set by server: %s\n", want[0] ? want : "auto");
	}
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
		/* новая прошивка — прервать файл (докачается после), обновиться */
		if (config_if_due() < 0 || ota.due) {
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

/* Сервер молчит столько — перезапускаем модем: зависший TLS (+MIPOPEN: 0,753) он сам не отпускает,
 * а перезагрузка ESP модем не трогает. */
#define MODEM_REBOOT_US (10LL * 60 * 1000000)

static void task(void *arg)
{
	int failures = 0;
	bool ready = false;
	int64_t reboot_us = 0;

	for (;;) {
		if (!cfg->token[0]) {
			set_state("token");
			printf("HBOX UP no token: type t <token> in the console\n");
			vTaskDelay(pdMS_TO_TICKS(30000));
			continue;
		}
		if (!ready || failures >= 5) {
			int64_t now = esp_timer_get_time();

			if (now - contact_us >= MODEM_REBOOT_US && now - reboot_us >= MODEM_REBOOT_US) {
				printf("HBOX UP no server for %lld min: modem reboot\n",
				       (long long)((now - contact_us) / 60000000));
				modem_at("AT+CFUN=1,1", NULL, NULL, 5000);
				vTaskDelay(pdMS_TO_TICKS(15000));
				reboot_us = now;
			}
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
		if (ota.due) {
			ota_run(); /* вернулась — не вышло, следующая попытка через OTA_RETRY_US */
			continue;
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
		} else if (!ota.due) {
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
	ota_init();
	xTaskCreatePinnedToCore(task, "uplink", 8192, NULL, 5, NULL, 0);
}
