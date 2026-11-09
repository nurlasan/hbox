/*
 * MP4-контейнер (.m4a) для AAC-LC без заголовков ADTS.
 *
 * Файл: ftyp, mdat (кадры подряд, пишутся по мере кодирования), moov в конце.
 * moov собирается из числа кадров и их размеров, поэтому после обрыва питания
 * файл можно дописать по сохранённой таблице размеров.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* ftyp + заголовок mdat; кадры начинаются сразу после них */
#define M4A_HEAD_SIZE 36
/* где в файле размер mdat (4 байта, big-endian) — дописать в конце */
#define M4A_MDAT_SIZE_OFFSET 28

struct m4a_info {
	uint32_t sample_rate;
	uint8_t channels;
	uint32_t bitrate;
	uint32_t frames; /* кадров по 1024 отсчёта */
	uint32_t data_bytes; /* сумма размеров кадров */
};

typedef void (*m4a_put_fn)(void *ctx, const void *data, size_t len);
/* размер очередного кадра, по порядку от первого */
typedef uint32_t (*m4a_next_size_fn)(void *ctx);

void m4a_head(uint8_t out[M4A_HEAD_SIZE]);
/* значение для M4A_MDAT_SIZE_OFFSET */
uint32_t m4a_mdat_size(const struct m4a_info *info);
size_t m4a_moov_size(const struct m4a_info *info);
void m4a_moov(const struct m4a_info *info, m4a_next_size_fn next_size, void *size_ctx,
	      m4a_put_fn put, void *put_ctx);
