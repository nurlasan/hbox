/* Выводы ESP32-S3 SuperMini (как в music-monitor). */
#pragma once

#define MIC_PIN_WS 4
#define MIC_PIN_SCK 5
#define MIC_PIN_SD 6

/* ML307R / A7670C на UART1; провода TX/RX прошивка находит сама (modem.c) */
#define MODEM_PIN_TX 12
#define MODEM_PIN_RX 13
