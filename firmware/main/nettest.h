/*
 * Тест скорости модема (hbox-xsg): TLS-сокет ML307R на наш сервер (server/speedtest.py),
 * 2 МБ случайных данных кусками по 8 КБ через AT+MIPSEND на UART 921600.
 * Печатает скорость приёма модемом и скорость, подтверждённую сервером (TCP ACK).
 */
#pragma once

void nettest_run(void);
