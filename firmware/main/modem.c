#include "modem.h"

#include <stdio.h>
#include <string.h>
#include <driver/uart.h>
#include <esp_timer.h>

#include "board.h"

#define UART UART_NUM_1

static int baud;
static bool swapped;

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
			return ESP_OK;
		}
		if (n == 0 && c == '>') {
			strcpy(line, ">");
			return ESP_OK;
		}
		if (n < MODEM_LINE_MAX - 1) {
			line[n++] = c;
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
		} else if (want && out && starts(line, want)) {
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
	return wait_ok(NULL, NULL, until);
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
	static const int rates[] = {115200, 921600};
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
		for (int i = 0; i < 4 && !found; i++) {
			set_port(rates[i % 2], i >= 2);
			found = probe();
		}
	}
	if (!found) {
		return ESP_ERR_NOT_FOUND;
	}
	modem_at("ATE0", NULL, NULL, 1000);
	if (baud != 921600) {
		/* новая скорость действует после ответа OK на эту команду */
		if (modem_at("AT+IPR=921600", NULL, NULL, 1000) != ESP_OK) {
			return ESP_OK; /* остаёмся на 115200 */
		}
		uart_wait_tx_done(UART, pdMS_TO_TICKS(100));
		vTaskDelay(pdMS_TO_TICKS(50));
		set_port(921600, swapped);
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
