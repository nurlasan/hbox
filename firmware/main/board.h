/* Выводы ESP32-S3 SuperMini: все GPIO устройства — здесь. */
#pragma once

#define MIC_PIN_WS 4
#define MIC_PIN_SCK 5
#define MIC_PIN_SD 6

/* ML307R / A7670C на UART1; провода TX/RX прошивка находит сама (modem.c) */
#define MODEM_PIN_TX 12
#define MODEM_PIN_RX 13

/* microSD по SPI (раскладка корпуса 2026-11) */
#define SD_PIN_CS 8
#define SD_PIN_SCK 9
#define SD_PIN_MOSI 10
#define SD_PIN_MISO 11
