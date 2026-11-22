#include "modem.h"

#include <stdio.h>
#include <string.h>
#include <driver/uart.h>
#include <esp_timer.h>

#include "board.h"

#define UART UART_NUM_1

static int baud;
static bool swapped;
/* SIMCom A7670C: другие команды сокетов и TLS (AT+CCH*), принятое читаем AT+CCHRECV */
static bool simcom;

static bool starts(const char *s, const char *prefix);

static int64_t deadline(int timeout_ms)
{
	return esp_timer_get_time() + (int64_t)timeout_ms * 1000;
}

static void tx(const void *data, size_t len)
{
	uart_write_bytes(UART, data, len);
}

static void tx_str(const char *s)
{
	tx(s, strlen(s));
}

/*
 * Принятые из сокета байты. Сокет открыт в режиме буфера (AT+MIPOPEN ...,2): модем держит принятое
 * у себя (до 64 КБ) и сообщает только +MIPURC: "rtcp",<id>,<пришло>,<всего в буфере>, а сами байты
 * мы забираем по 4 КБ командой AT+MIPRD=<id>,4096 — ответ +MIPRD: <id>,<осталось>,<длина>,<байты>.
 * read_line вынимает байты сюда. Раньше модем сам выталкивал всё принятое одним +MIPURC, и ответ
 * больше 16 КБ не помещался (rx overflow).
 */
#define SOCK_RX 16384
static uint8_t sock_rx[SOCK_RX];
static size_t rx_head, rx_tail;
static volatile bool sock_closed;
/* сколько байт ждёт в буфере модема (по +MIPURC "rtcp" и +MIPRD); у A7670C — 1, если что-то есть */
static volatile size_t sock_unread;
#define READ_CHUNK 4096

static void sock_put(const uint8_t *p, size_t n)
{
	while (n--) {
		size_t next = (rx_head + 1) % SOCK_RX;

		if (next == rx_tail) {
			/* ответ больше буфера — разобрать его не выйдет, запрос повторится */
			printf("HBOX MODEM rx overflow, %u bytes lost\n", (unsigned)(n + 1));
			return;
		}
		sock_rx[rx_head] = *p++;
		rx_head = next;
	}
}

/* +MIPURC: "rtcp",<id>,<пришло>,<всего> — сколько лежит в буфере модема */
static void note_rtcp(const char *line)
{
	int id, got, total;

	if (sscanf(line, "+MIPURC: \"rtcp\",%d,%d,%d", &id, &got, &total) == 3) {
		sock_unread = total;
	}
}

/* +MIPRD: <id>,<осталось>,<длина>, — после третьей запятой идут сырые байты указанной длины */
static bool take_miprd(const char *line, size_t n, int64_t until)
{
	static const char prefix[] = "+MIPRD: ";
	int id, left, len, commas = 0;
	uint8_t buf[256];

	if (line[n - 1] != ',' || n < sizeof(prefix) || strncmp(line, prefix, sizeof(prefix) - 1)) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		commas += line[i] == ',';
	}
	if (commas != 3 || sscanf(line + sizeof(prefix) - 1, "%d,%d,%d,", &id, &left, &len) != 3) {
		return false;
	}
	sock_unread = left;
	while (len > 0) {
		int64_t wait = until - esp_timer_get_time();
		int got = uart_read_bytes(UART, buf, len < (int)sizeof(buf) ? len : (int)sizeof(buf),
					  pdMS_TO_TICKS((wait > 1000000 ? wait : 1000000) / 1000));

		if (got <= 0) {
			break;
		}
		sock_put(buf, got);
		len -= got;
	}
	return true;
}

/* +CCHRECV: DATA,<id>,<длина> — следом сырые байты (A7670C) */
static void take_cchrecv(const char *line, int64_t until)
{
	int id, len;
	uint8_t buf[256];

	if (sscanf(line, "+CCHRECV: DATA,%d,%d", &id, &len) != 2) {
		return;
	}
	while (len > 0) {
		int64_t wait = until - esp_timer_get_time();
		int got = uart_read_bytes(UART, buf, len < (int)sizeof(buf) ? len : (int)sizeof(buf),
					  pdMS_TO_TICKS((wait > 1000000 ? wait : 1000000) / 1000));

		if (got <= 0) {
			break;
		}
		sock_put(buf, got);
		len -= got;
	}
}

/* Строка без \r\n; приглашение '>' приходит без перевода строки — отдаётся сразу. */
static esp_err_t read_line(char *line, int64_t until)
{
	size_t n = 0;

	for (;;) {
		int64_t left = until - esp_timer_get_time();
		uint8_t c;

		if (left <= 0 || uart_read_bytes(UART, &c, 1, pdMS_TO_TICKS(left / 1000 + 1)) != 1) {
			if (esp_timer_get_time() >= until) {
				return ESP_ERR_TIMEOUT;
			}
			continue;
		}
		if (c == '\r') {
			continue;
		}
		if (c == '\n') {
			if (n == 0) {
				continue;
			}
			line[n] = '\0';
			/* +CCHCLOSE: — ответ на наше закрытие прежнего соединения, приходит с опозданием (уже
			 * после открытия нового) — не обрыв */
			if (starts(line, "+MIPURC: \"disconn\"") || starts(line, "+CCH_PEER_CLOSED:") ||
			    starts(line, "+CCH_RECV_CLOSED:")) {
				sock_closed = true;
			} else if (starts(line, "+MIPURC: \"rtcp\"")) {
				note_rtcp(line);
			} else if (starts(line, "+CCHEVENT:") && strstr(line, "RECV EVENT")) {
				sock_unread = 1;
			} else if (starts(line, "+CCHRECV: DATA,")) {
				take_cchrecv(line, until);
				line[0] = '\0';
			}
			return ESP_OK;
		}
		if (n == 0 && c == '>') {
			strcpy(line, ">");
			return ESP_OK;
		}
		if (n < MODEM_LINE_MAX - 1) {
			line[n++] = c;
			line[n] = '\0';
			/* данные сокета забраны — отдаём пустую строку, чтобы ждущий их сразу увидел */
			if (c == ',' && take_miprd(line, n, until)) {
				line[0] = '\0';
				return ESP_OK;
			}
		}
	}
}

static bool starts(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_error(const char *line)
{
	return strcmp(line, "ERROR") == 0 || starts(line, "+CME ERROR");
}

static esp_err_t wait_ok(const char *want, char *out, int64_t until)
{
	char line[MODEM_LINE_MAX];

	while (read_line(line, until) == ESP_OK) {
		if (strcmp(line, "OK") == 0) {
			return ESP_OK;
		} else if (is_error(line)) {
			if (out) {
				strcpy(out, line);
			}
			return ESP_FAIL;
		} else if (want && out && line[0] && starts(line, want)) {
			strcpy(out, line);
		}
	}
	return ESP_ERR_TIMEOUT;
}

esp_err_t modem_at(const char *cmd, const char *want, char *out, int timeout_ms)
{
	tx_str(cmd);
	tx_str("\r\n");
	return wait_ok(want, out, deadline(timeout_ms));
}

esp_err_t modem_at_data(const char *cmd, const void *data, size_t len, int timeout_ms)
{
	int64_t until = deadline(timeout_ms);
	char line[MODEM_LINE_MAX];

	tx_str(cmd);
	tx_str("\r\n");
	for (;;) {
		if (read_line(line, until) != ESP_OK) {
			return ESP_ERR_TIMEOUT;
		}
		if (strcmp(line, ">") == 0) {
			break;
		}
		if (is_error(line)) {
			return ESP_FAIL;
		}
	}
	tx(data, len);
	esp_err_t err = wait_ok(NULL, NULL, until);

	if (err == ESP_ERR_TIMEOUT) {
		/* модем недополучил байты и всё ещё ждёт данные: ESC отменяет ввод */
		tx("\x1b", 1);
		vTaskDelay(pdMS_TO_TICKS(1000));
		uart_flush_input(UART);
	}
	return err;
}

esp_err_t modem_wait(const char *prefix, char *out, int timeout_ms)
{
	int64_t until = deadline(timeout_ms);
	char line[MODEM_LINE_MAX];

	while (read_line(line, until) == ESP_OK) {
		if (starts(line, prefix)) {
			strcpy(out, line);
			return ESP_OK;
		}
	}
	return ESP_ERR_TIMEOUT;
}

static void set_port(int rate, bool swap)
{
	swapped = swap;
	uart_set_baudrate(UART, rate);
	uart_set_pin(UART, swapped ? MODEM_PIN_RX : MODEM_PIN_TX, swapped ? MODEM_PIN_TX : MODEM_PIN_RX,
		     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
	uart_flush_input(UART);
	baud = rate;
}

/* 3 «AT» подряд: модем с автоподбором скорости ловит её по первым символам */
static bool probe(void)
{
	for (int i = 0; i < 3; i++) {
		if (modem_at("AT", NULL, NULL, 300) == ESP_OK) {
			return true;
		}
	}
	return false;
}

esp_err_t modem_open(void)
{
	static const int rates[] = {115200, 921600, MODEM_BAUD};
	uart_config_t cfg = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity = UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};
	bool found = false;

	if (!uart_is_driver_installed(UART)) {
		ESP_ERROR_CHECK(uart_driver_install(UART, 16384, 0, 0, NULL, 0));
		ESP_ERROR_CHECK(uart_param_config(UART, &cfg));
	}
	/* модем после включения просыпается до ~10 с */
	for (int round = 0; round < 5 && !found; round++) {
		for (int i = 0; i < 6 && !found; i++) {
			set_port(rates[i % 3], i >= 3);
			found = probe();
		}
	}
	if (!found) {
		return ESP_ERR_NOT_FOUND;
	}
	modem_at("ATE0", NULL, NULL, 1000);
	char model[MODEM_LINE_MAX] = "";

	modem_at("AT+CGMM", "", model, 1000);
	simcom = strstr(model, "A76") || strstr(model, "A78") || strstr(model, "SIMCOM");
	printf("HBOX MODEM model %s (%s)\n", model[0] ? model : "?", simcom ? "SIMCom" : "ML307");
	if (baud != MODEM_BAUD) {
		char cmd[24];

		/* новая скорость действует после ответа OK на эту команду */
		snprintf(cmd, sizeof(cmd), "AT+IPR=%d", MODEM_BAUD);
		if (modem_at(cmd, NULL, NULL, 1000) != ESP_OK) {
			return ESP_OK; /* остаёмся на 115200 */
		}
		uart_wait_tx_done(UART, pdMS_TO_TICKS(100));
		vTaskDelay(pdMS_TO_TICKS(50));
		set_port(MODEM_BAUD, swapped);
		if (!probe()) {
			return ESP_ERR_INVALID_RESPONSE;
		}
	}
	return ESP_OK;
}

int modem_baud(void)
{
	return baud;
}

bool modem_swapped(void)
{
	return swapped;
}

bool modem_simcom(void)
{
	return simcom;
}

esp_err_t modem_net_up(void)
{
	char line[MODEM_LINE_MAX] = "";

	if (simcom) {
		/* AT+CCHSTART сам поднимает интернет; принятое — вручную (CCHSET ...,1), до CCHSTART */
		modem_at("AT+CCHSET=0,1", NULL, NULL, 2000);
		if (modem_at("AT+CCHSTART", NULL, NULL, 5000) == ESP_OK) {
			modem_wait("+CCHSTART:", line, 30000);
		}
		return ESP_OK; /* уже запущен — ERROR, это не ошибка */
	}
	/* ML307R: мобильный интернет обычно включается сам */
	modem_at("AT+MIPCALL?", "+MIPCALL:", line, 2000);
	if (!strstr(line, "+MIPCALL: 1,1")) {
		modem_at("AT+MIPCALL=1,1", NULL, NULL, 5000);
		modem_wait("+MIPCALL:", line, 30000);
	}
	return ESP_OK;
}

esp_err_t modem_tls_cert(int ssl_id, const char *name, const char *pem, bool sni)
{
	char cmd[96];
	size_t len = strlen(pem);
	esp_err_t err;

	if (simcom) {
		snprintf(cmd, sizeof(cmd), "AT+CCERTDOWN=\"%s\",%u", name, (unsigned)len);
		err = modem_at_data(cmd, pem, len, 10000);
		/* только TLS 1.2: с 4 («все», вместе с SSL3) A7670C-LANV рвал соединение до ClientHello */
		snprintf(cmd, sizeof(cmd), "AT+CSSLCFG=\"sslversion\",%d,3", ssl_id);
		err = err ?: modem_at(cmd, NULL, NULL, 2000);
		snprintf(cmd, sizeof(cmd), "AT+CSSLCFG=\"authmode\",%d,1", ssl_id);
		err = err ?: modem_at(cmd, NULL, NULL, 2000);
		snprintf(cmd, sizeof(cmd), "AT+CSSLCFG=\"cacert\",%d,\"%s\"", ssl_id, name);
		err = err ?: modem_at(cmd, NULL, NULL, 2000);
		/* часы модема не выставлены — срок сертификата по ним не проверять (иначе +CCHOPEN: 0,15) */
		snprintf(cmd, sizeof(cmd), "AT+CSSLCFG=\"ignorelocaltime\",%d,1", ssl_id);
		modem_at(cmd, NULL, NULL, 2000);
		/* имя сервера — только для Cloudflare; напрямую адрес IP */
		snprintf(cmd, sizeof(cmd), "AT+CSSLCFG=\"enableSNI\",%d,%d", ssl_id, sni);
		modem_at(cmd, NULL, NULL, 2000);
		return err;
	}
	/* Сертификат одним куском, как в примере документации: запись кусками (remain_length) модем
	 * не принял (9f13f54), а 6 КБ одним куском — тоже (f346022). Поэтому у Cloudflare один корень. */
	snprintf(cmd, sizeof(cmd), "AT+MSSLCERTWR=\"%s\",0,%u", name, (unsigned)len);
	err = modem_at_data(cmd, pem, len, 5000);
	snprintf(cmd, sizeof(cmd), "AT+MSSLCFG=\"auth\",%d,1", ssl_id);
	err = err ?: modem_at(cmd, NULL, NULL, 2000);
	snprintf(cmd, sizeof(cmd), "AT+MSSLCFG=\"cert\",%d,\"%s\"", ssl_id, name);
	err = err ?: modem_at(cmd, NULL, NULL, 2000);
	return err;
}

/* Ответ модема на команду целиком — в консоль (диагностика A7670C). */
static void at_show(const char *cmd)
{
	char line[MODEM_LINE_MAX];
	int64_t until = deadline(3000);

	tx_str(cmd);
	tx_str("\r\n");
	while (read_line(line, until) == ESP_OK) {
		if (line[0]) {
			printf("HBOX MODEM %s: %s\n", cmd, line);
		}
		if (!strcmp(line, "OK") || is_error(line)) {
			break;
		}
	}
}

/* A7670C не смог TLS (15): один раз — что знает модем и откроется ли без проверки сертификата
 * (соединение сразу закрывается, ничего не шлём) */
static void tls_diag(int id, const char *host, int port)
{
	char cmd[96], line[MODEM_LINE_MAX] = "";

	at_show("AT+CCERTLIST");
	at_show("AT+CCLK?");
	at_show("AT+CSSLCFG?");
	modem_at("AT+CSSLCFG=\"sslversion\",9,3", NULL, NULL, 2000);
	modem_at("AT+CSSLCFG=\"authmode\",9,0", NULL, NULL, 2000);
	snprintf(cmd, sizeof(cmd), "AT+CCHSSLCFG=%d,9", id);
	modem_at(cmd, NULL, NULL, 2000);
	snprintf(cmd, sizeof(cmd), "AT+CCHOPEN=%d,\"%s\",%d,2", id, host, port);
	if (modem_at(cmd, NULL, line, 5000) == ESP_OK) {
		modem_wait("+CCHOPEN:", line, 70000);
	}
	printf("HBOX MODEM diag without cert check: %s\n", line);
	snprintf(cmd, sizeof(cmd), "AT+CCHCLOSE=%d", id);
	modem_at(cmd, NULL, NULL, 5000);
}

esp_err_t modem_sock_open(int id, int ssl_id, const char *host, int port, char *line)
{
	static bool diag_done;

	char cmd[96];

	line[0] = '\0';
	if (simcom) {
		snprintf(cmd, sizeof(cmd), "AT+CCHSSLCFG=%d,%d", id, ssl_id);
		modem_at(cmd, NULL, NULL, 2000);
		snprintf(cmd, sizeof(cmd), "AT+CCHOPEN=%d,\"%s\",%d,2", id, host, port);
		if (modem_at(cmd, NULL, line, 5000) != ESP_OK || modem_wait("+CCHOPEN:", line, 70000) != ESP_OK) {
			return ESP_FAIL;
		}
		/* +CCHOPEN: <id>,0 — открыт */
		int sid, err = -1;

		if (sscanf(line, "+CCHOPEN: %d,%d", &sid, &err) == 2 && err == 0) {
			return ESP_OK;
		}
		if (err == 15 && !diag_done) {
			diag_done = true;
			char keep[MODEM_LINE_MAX];

			strcpy(keep, line);
			tls_diag(id, host, port);
			strcpy(line, keep);
		}
		return ESP_FAIL;
	}
	snprintf(cmd, sizeof(cmd), "AT+MIPCFG=\"ssl\",%d,1,%d", id, ssl_id);
	modem_at(cmd, NULL, NULL, 2000);
	/* режим буфера (,2): принятое ждёт в модеме, забираем AT+MIPRD */
	snprintf(cmd, sizeof(cmd), "AT+MIPOPEN=%d,\"TCP\",\"%s\",%d,60,2", id, host, port);
	if (modem_at(cmd, NULL, line, 5000) != ESP_OK || modem_wait("+MIPOPEN:", line, 70000) != ESP_OK ||
	    !strstr(line, ",0")) {
		return ESP_FAIL;
	}
	return ESP_OK;
}

void modem_sock_close(int id)
{
	char cmd[24];

	snprintf(cmd, sizeof(cmd), simcom ? "AT+CCHCLOSE=%d" : "AT+MIPCLOSE=%d", id);
	modem_at(cmd, NULL, NULL, 5000);
}

void modem_sock_reset(void)
{
	rx_head = rx_tail = 0;
	sock_unread = 0;
	sock_closed = false;
}

bool modem_sock_closed(void)
{
	return sock_closed && rx_head == rx_tail && !sock_unread;
}

esp_err_t modem_sock_send(int id, const void *data, size_t len, int timeout_ms)
{
	const uint8_t *p = data;
	char cmd[32];

	while (len) {
		size_t max = simcom ? 2048 : MODEM_SEND_MAX; /* AT+CCHSEND берёт до 2048 */
		size_t n = len < max ? len : max;
		esp_err_t err;

		if (sock_closed) {
			return ESP_ERR_INVALID_STATE;
		}
		snprintf(cmd, sizeof(cmd), simcom ? "AT+CCHSEND=%d,%u" : "AT+MIPSEND=%d,%u", id, (unsigned)n);
		err = modem_at_data(cmd, p, n, timeout_ms);
		if (err) {
			return err;
		}
		p += n;
		len -= n;
	}
	return ESP_OK;
}

size_t modem_sock_recv(void *buf, size_t len, int timeout_ms)
{
	int64_t until = deadline(timeout_ms);
	char line[MODEM_LINE_MAX];
	uint8_t *p = buf;
	size_t n = 0;

	/* пусто — забрать из буфера модема; там тоже пусто — ждать +MIPURC "rtcp" */
	while (rx_head == rx_tail && esp_timer_get_time() < until) {
		if (sock_unread) {
			char cmd[32];

			size_t before = (rx_head + SOCK_RX - rx_tail) % SOCK_RX;

			snprintf(cmd, sizeof(cmd), simcom ? "AT+CCHRECV=0,%d" : "AT+MIPRD=0,%d", READ_CHUNK);
			if (modem_at(cmd, NULL, NULL, 5000) != ESP_OK) {
				if (simcom) {
					sock_unread = 0; /* ERROR — в буфере пусто */
				}
				break;
			}
			size_t got = (rx_head + SOCK_RX - rx_tail) % SOCK_RX - before;

			if (rx_head == rx_tail || (simcom && got < READ_CHUNK)) {
				sock_unread = 0; /* модем говорит, что читать нечего; A7670C отдал меньше — всё */
			}
			continue;
		}
		if (sock_closed || read_line(line, until) != ESP_OK) {
			break;
		}
	}
	while (n < len && rx_tail != rx_head) {
		p[n++] = sock_rx[rx_tail];
		rx_tail = (rx_tail + 1) % SOCK_RX;
	}
	return n;
}
