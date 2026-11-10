#include "modem.h"

#include <stdio.h>
#include <string.h>
#include <driver/uart.h>
#include <esp_timer.h>

#include "board.h"

#define UART UART_NUM_1

static int baud;
static bool swapped;

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
 * Принятые из сокета байты. Модем выдаёт их посреди ответов на AT-команды как
 * +MIPURC: "rtcp",<id>,<длина>,<байты> — read_line вынимает их сюда, где бы они ни пришли.
 */
#define SOCK_RX 16384
static uint8_t sock_rx[SOCK_RX];
static size_t rx_head, rx_tail;
static volatile bool sock_closed;

static void sock_put(const uint8_t *p, size_t n)
{
	while (n--) {
		size_t next = (rx_head + 1) % SOCK_RX;

		if (next == rx_tail) {
			return; /* ответы сервера маленькие; переполнение — ответ не разберётся */
		}
		sock_rx[rx_head] = *p++;
		rx_head = next;
	}
}

/* После третьей запятой в строке +MIPURC: "rtcp" идут сырые байты указанной длины. */
static bool take_rtcp(const char *line, size_t n, int64_t until)
{
	static const char prefix[] = "+MIPURC: \"rtcp\",";
	int id, len, commas = 0;
	uint8_t buf[256];

	if (line[n - 1] != ',' || n < sizeof(prefix) || strncmp(line, prefix, sizeof(prefix) - 1)) {
		return false;
	}
	for (size_t i = 0; i < n; i++) {
		commas += line[i] == ',';
	}
	if (commas != 3 || sscanf(line + sizeof(prefix) - 1, "%d,%d,", &id, &len) != 2) {
		return false;
	}
	while (len > 0) {
		int64_t left = until - esp_timer_get_time();
		int got = uart_read_bytes(UART, buf, len < (int)sizeof(buf) ? len : (int)sizeof(buf),
					  pdMS_TO_TICKS((left > 1000000 ? left : 1000000) / 1000));

		if (got <= 0) {
			break;
		}
		sock_put(buf, got);
		len -= got;
	}
	return true;
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
			if (starts(line, "+MIPURC: \"disconn\"")) {
				sock_closed = true;
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
			if (c == ',' && take_rtcp(line, n, until)) {
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

void modem_sock_reset(void)
{
	rx_head = rx_tail = 0;
	sock_closed = false;
}

bool modem_sock_closed(void)
{
	return sock_closed && rx_head == rx_tail;
}

esp_err_t modem_sock_send(int id, const void *data, size_t len, int timeout_ms)
{
	const uint8_t *p = data;
	char cmd[32];

	while (len) {
		size_t n = len < MODEM_SEND_MAX ? len : MODEM_SEND_MAX;
		esp_err_t err;

		if (sock_closed) {
			return ESP_ERR_INVALID_STATE;
		}
		snprintf(cmd, sizeof(cmd), "AT+MIPSEND=%d,%u", id, (unsigned)n);
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

	/* пусто — читаем поток модема: read_line сам складывает данные сокета в sock_rx */
	while (rx_head == rx_tail && !sock_closed && read_line(line, until) == ESP_OK) {
	}
	while (n < len && rx_tail != rx_head) {
		p[n++] = sock_rx[rx_tail];
		rx_tail = (rx_tail + 1) % SOCK_RX;
	}
	return n;
}
