/*
 * Кодирование в файл хранилища: MP3 (shine) или AAC-LC в .m4a (esp_audio_codec + m4a.c),
 * моно 44.1 кГц. Один файл за раз.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store.h"

/* "mp3" или "m4a"; другое — mp3 */
const char *enc_ext(const char *format);
/* Открывает кодировщик; возвращает отсчётов на кадр (0 — ошибка). */
size_t enc_start(struct store_file *f, const char *ext, int bitrate);
/* Один кадр PCM (enc_start отсчётов). false — хранилище переполнено или ошибка кодека. */
bool enc_frame(int16_t *pcm);
/* Дописывает хвост и контейнер; файл ещё надо закрыть store_close. */
void enc_finish(void);
