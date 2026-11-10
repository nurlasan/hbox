/*
 * Связь с сервером через модем ML307R: настройки записи (GET /hbox/v1/config) и отправка
 * готовых файлов с докачкой (docs/protocol.md). Своя задача на ядре 0.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

void uplink_start(struct settings *s);

struct uplink_stats {
	const char *state; /* "token", "modem", "net", "ppp", "idle", "send", "ota", "wait" */
	int csq;
	uint32_t sent_files;
	uint32_t sent_kb;
	uint32_t kbps10; /* скорость последней отправки, десятые КБ/с */
	uint32_t errors;
};

void uplink_get_stats(struct uplink_stats *st);
