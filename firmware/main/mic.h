/* INMP441 по I2S: моно (L/R → GND), int16 после ФВЧ 40 Гц и усиления. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <esp_err.h>

#define MIC_SAMPLE_RATE 44100

esp_err_t mic_start(void);
void mic_stop(void);
/* Ровно n отсчётов в out; ждёт, пока DMA их наберёт. */
esp_err_t mic_read(int16_t *out, size_t n);
/* Переполнения очереди DMA с начала работы: >0 — потеряли звук, кодировщик не успевает. */
uint32_t mic_overflows(void);
