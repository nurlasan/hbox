/*
 * AT-команды модема ML307R по UART1 (перенос из music-monitor uplink.c на ESP-IDF).
 * Скорость и перепутанные провода TX/RX подбираются при modem_open().
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <esp_err.h>

#define MODEM_LINE_MAX 160

/* Находит модем (115200 или 921600, провода в обе стороны) и переводит порт на 921600. */
esp_err_t modem_open(void);
int modem_baud(void);
/* провода TX/RX оказались перепутаны — выводы поменяны местами */
bool modem_swapped(void);
/*
 * Команда и ожидание OK/ERROR. Если want задан, строка с этим префиксом копируется в out
 * ("" — любая строка ответа).
 * ESP_OK, ESP_FAIL — ERROR, ESP_ERR_TIMEOUT — нет ответа.
 */
esp_err_t modem_at(const char *cmd, const char *want, char *out, int timeout_ms);
/* Команда с приглашением '>' и затем данные; ждёт OK. */
esp_err_t modem_at_data(const char *cmd, const void *data, size_t len, int timeout_ms);
/* Ждёт строку с префиксом (URC), остальные пропускает. */
esp_err_t modem_wait(const char *prefix, char *out, int timeout_ms);

/* --- TCP/TLS-сокет ML307R (AT+MIPOPEN — у вызывающего) --- */

/* больше AT+MIPSEND за раз не берёт */
#define MODEM_SEND_MAX 8192

/* Перед открытием сокета: очистить принятое и флаг разрыва. */
void modem_sock_reset(void);
/* Модем сообщил о разрыве и всё принятое прочитано. */
bool modem_sock_closed(void);
/* Данные в сокет кусками по MODEM_SEND_MAX; ESP_OK — модем принял в свой буфер. */
esp_err_t modem_sock_send(int id, const void *data, size_t len, int timeout_ms);
/* До len принятых байт; ждёт до timeout_ms, если пока ничего нет. 0 — нет данных. */
size_t modem_sock_recv(void *buf, size_t len, int timeout_ms);

