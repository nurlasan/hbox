#include "nettest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "modem.h"

#define SERVER "178.104.149.56"
/* порт прокси music-monitor: другие у сервера закрыты, /hbox/ он отдаёт серверу Hbox */
#define PORT 18443
#define TOTAL (2 * 1024 * 1024)
#define CHUNK 8192 /* больше AT+MIPSEND не берёт */
#define LIMIT_S 300

static uint8_t data[CHUNK];

static double seconds_since(int64_t t0)
{
	return (esp_timer_get_time() - t0) / 1e6;
}

static bool registered(char *line)
{
	for (int i = 0; i < 60; i++) {
		line[0] = '\0';
		if (modem_at("AT+CEREG?", "+CEREG:", line, 2000) == ESP_OK &&
		    (strstr(line, ",1") || strstr(line, ",5"))) {
			return true;
		}
		vTaskDelay(pdMS_TO_TICKS(2000));
	}
	return false;
}

/* +MIPSACK: <sent>,<acked>,<nack>,<received> */
static bool sack(long *sent, long *acked)
{
	char line[MODEM_LINE_MAX] = "";

	return modem_at("AT+MIPSACK=0", "+MIPSACK:", line, 2000) == ESP_OK &&
	       sscanf(line, "+MIPSACK: %ld,%ld", sent, acked) == 2;
}

void nettest_run(void)
{
	char line[MODEM_LINE_MAX], cmd[96];
	long total = 0, sent = 0, acked = 0;
	int errors = 0, streak = 0;
	esp_err_t err;
	int64_t t0;
	double push_s, ack_s;

	printf("HBOX NET start\n");
	err = modem_open();
	if (err) {
		printf("HBOX NET ERROR modem %s\n", esp_err_to_name(err));
		return;
	}
	line[0] = '\0';
	modem_at("AT+CGMM", "", line, 1000);
	printf("HBOX NET modem=%s baud=%d swapped=%d\n", line, modem_baud(), modem_swapped());
	if (!registered(line)) {
		printf("HBOX NET ERROR no network (%s)\n", line);
		return;
	}
	line[0] = '\0';
	modem_at("AT+CSQ", "+CSQ:", line, 1000);
	printf("HBOX NET %s\n", line);
	line[0] = '\0';
	modem_at("AT+COPS?", "+COPS:", line, 2000);
	printf("HBOX NET %s\n", line);

	/* мобильный интернет (PDP): обычно включается сам, иначе — вручную */
	line[0] = '\0';
	modem_at("AT+MIPCALL?", "+MIPCALL:", line, 2000);
	if (!strstr(line, "+MIPCALL: 1,1")) {
		modem_at("AT+MIPCALL=1,1", NULL, NULL, 5000);
		modem_wait("+MIPCALL:", line, 30000);
	}
	printf("HBOX NET %s\n", line);

	modem_at("AT+MIPCLOSE=0", NULL, NULL, 5000);
	/* тест: TLS без проверки сертификата; в рабочей прошивке — проверка по нашему CA */
	modem_at("AT+MSSLCFG=\"auth\",1,0", NULL, NULL, 2000);
	modem_at("AT+MIPCFG=\"ssl\",0,1,1", NULL, NULL, 2000);
	t0 = esp_timer_get_time();
	snprintf(cmd, sizeof(cmd), "AT+MIPOPEN=0,\"TCP\",\"%s\",%d,60,0", SERVER, PORT);
	line[0] = '\0';
	if (modem_at(cmd, NULL, line, 5000) != ESP_OK ||
	    modem_wait("+MIPOPEN: 0,", line, 70000) != ESP_OK || strcmp(line, "+MIPOPEN: 0,0") != 0) {
		printf("HBOX NET ERROR open %s\n", line);
		/* без TLS: если откроется — мешает TLS, если нет — не доходит до сервера */
		modem_at("AT+MIPCLOSE=0", NULL, NULL, 5000);
		modem_at("AT+MIPCFG=\"ssl\",0,0", NULL, NULL, 2000);
		line[0] = '\0';
		if (modem_at(cmd, NULL, line, 5000) == ESP_OK) {
			modem_wait("+MIPOPEN: 0,", line, 70000);
		}
		printf("HBOX NET plain tcp: %s\n", line);
		modem_at("AT+MIPCLOSE=0", NULL, NULL, 5000);
		return;
	}
	printf("HBOX NET connected tls in %.1f s\n", seconds_since(t0));

	/* HTTP-запрос с телом TOTAL байт: заголовок, затем тело кусками */
	snprintf((char *)data, sizeof(data),
		 "POST /hbox/speedtest HTTP/1.1\r\nHost: %s\r\nContent-Type: application/octet-stream\r\n"
		 "Content-Length: %d\r\nConnection: close\r\n\r\n",
		 SERVER, TOTAL);
	snprintf(cmd, sizeof(cmd), "AT+MIPSEND=0,%d", (int)strlen((char *)data));
	if (modem_at_data(cmd, data, strlen((char *)data), 30000) != ESP_OK) {
		printf("HBOX NET ERROR send header\n");
		modem_at("AT+MIPCLOSE=0", NULL, NULL, 5000);
		return;
	}
	esp_fill_random(data, sizeof(data));
	snprintf(cmd, sizeof(cmd), "AT+MIPSEND=0,%d", CHUNK);
	t0 = esp_timer_get_time();
	while (total < TOTAL && seconds_since(t0) < LIMIT_S && streak < 10) {
		if (modem_at_data(cmd, data, CHUNK, 30000) == ESP_OK) {
			total += CHUNK;
			streak = 0;
			if (total % (256 * 1024) == 0) {
				printf("HBOX NET sent %ld KB, %.1f KB/s\n", total / 1024,
				       total / 1024.0 / seconds_since(t0));
			}
		} else {
			/* буфер модема полон или связь оборвалась — пауза и снова */
			errors++;
			streak++;
			vTaskDelay(pdMS_TO_TICKS(200));
		}
	}
	push_s = seconds_since(t0);
	/* сколько дошло до сервера: ждём, пока все отправленные байты подтвердятся */
	while (sack(&sent, &acked) && acked < sent && seconds_since(t0) < LIMIT_S + 120) {
		vTaskDelay(pdMS_TO_TICKS(500));
	}
	ack_s = seconds_since(t0);
	printf("HBOX NET result bytes=%ld push=%.1fKB/s acked=%ld in %.1f s = %.1fKB/s errors=%d "
	       "need=16KB/s\n",
	       total, total / 1024.0 / push_s, acked, ack_s, acked / 1024.0 / ack_s, errors);
	modem_at("AT+MIPCLOSE=0", NULL, NULL, 10000);
}
