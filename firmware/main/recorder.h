/*
 * Запись: микрофон → кодировщик → файл хранилища. Новый файл — на границе часа UTC
 * (пока карты нет — каждые STORE_FILE_SECONDS); до установки времени — по длительности.
 * Формат, битрейт и вкл/выкл применяются с начала следующего файла.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void recorder_start(const char *format, int bitrate, bool record);
void recorder_configure(const char *format, int bitrate, bool record);

struct recorder_stats {
	bool recording;
	uint32_t files;     /* закрыто файлов с включения */
	uint32_t errors;    /* сбои микрофона/кодека/хранилища */
	uint32_t overflows; /* потери звука: кодировщик не успел */
	uint32_t cpu10;     /* загрузка ядра кодированием, десятые доли процента */
};

void recorder_get_stats(struct recorder_stats *st);
