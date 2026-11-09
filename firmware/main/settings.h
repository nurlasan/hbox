/*
 * Настройки в NVS: токен устройства (вводится по USB, в прошивке его нет) и последние
 * настройки записи с сервера — чтобы после перезагрузки писать как раньше ещё до связи.
 */
#pragma once

#include <stdbool.h>

struct settings {
	char token[65];
	char channel[33]; /* id устройства на сервере, он же префикс имени файла */
	char format[4];   /* "mp3" / "m4a" */
	int bitrate;
	bool record;
};

void settings_load(struct settings *s);
void settings_save(const struct settings *s);
