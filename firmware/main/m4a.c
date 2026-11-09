#include "m4a.h"

#include <string.h>

#define FRAME_SAMPLES 1024

/* размеры боксов moov без таблицы размеров кадров (stsz: +4 байта на кадр) */
#define ESDS 39
#define MP4A (36 + ESDS)
#define STSD (16 + MP4A)
#define STTS 24
#define STSC 28
#define STSZ(n) (20 + 4 * (n))
#define STCO 20
#define STBL(n) (8 + STSD + STTS + STSC + STSZ(n) + STCO)
#define SMHD 16
#define DINF 36
#define MINF(n) (8 + SMHD + DINF + STBL(n))
#define MDHD 32
#define HDLR 45
#define MDIA(n) (8 + MDHD + HDLR + MINF(n))
#define TKHD 92
#define TRAK(n) (8 + TKHD + MDIA(n))
#define MVHD 108
#define MOOV(n) (8 + MVHD + TRAK(n))

/* частоты MPEG-4 Audio по индексу (AudioSpecificConfig) */
static const uint32_t rates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
				 22050, 16000, 12000, 11025, 8000, 7350};

struct out {
	uint8_t buf[128];
	size_t len;
	m4a_put_fn put;
	void *ctx;
};

static void flush(struct out *o)
{
	o->put(o->ctx, o->buf, o->len);
	o->len = 0;
}

static void u8(struct out *o, uint8_t v)
{
	if (o->len == sizeof(o->buf)) {
		flush(o);
	}
	o->buf[o->len++] = v;
}

static void u16(struct out *o, uint16_t v)
{
	u8(o, v >> 8);
	u8(o, v);
}

static void u32(struct out *o, uint32_t v)
{
	u16(o, v >> 16);
	u16(o, v);
}

static void zeros(struct out *o, size_t n)
{
	while (n--) {
		u8(o, 0);
	}
}

static void fourcc(struct out *o, const char *type)
{
	for (int i = 0; i < 4; i++) {
		u8(o, type[i]);
	}
}

static void box(struct out *o, uint32_t size, const char *type)
{
	u32(o, size);
	fourcc(o, type);
}

/* полный бокс: + версия и флаги */
static void fbox(struct out *o, uint32_t size, const char *type, uint32_t flags)
{
	box(o, size, type);
	u32(o, flags);
}

static void matrix(struct out *o)
{
	static const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};

	for (int i = 0; i < 9; i++) {
		u32(o, m[i]);
	}
}

void m4a_head(uint8_t out[M4A_HEAD_SIZE])
{
	static const uint8_t head[M4A_HEAD_SIZE] = {
		0, 0, 0, 28, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ', 0, 0, 0, 0,
		'M', '4', 'A', ' ', 'i', 's', 'o', 'm', 'm', 'p', '4', '2',
		0, 0, 0, 8, 'm', 'd', 'a', 't',
	};

	memcpy(out, head, sizeof(head));
}

uint32_t m4a_mdat_size(const struct m4a_info *info)
{
	return 8 + info->data_bytes;
}

size_t m4a_moov_size(const struct m4a_info *info)
{
	return MOOV(info->frames);
}

void m4a_moov(const struct m4a_info *info, m4a_next_size_fn next_size, void *size_ctx,
	      m4a_put_fn put, void *put_ctx)
{
	struct out o = {.put = put, .ctx = put_ctx};
	uint32_t n = info->frames;
	uint32_t samples = n * FRAME_SAMPLES;
	uint32_t ms = (uint32_t)((uint64_t)samples * 1000 / info->sample_rate);
	uint8_t idx = 15;

	for (uint8_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		if (rates[i] == info->sample_rate) {
			idx = i;
		}
	}

	box(&o, MOOV(n), "moov");

	fbox(&o, MVHD, "mvhd", 0);
	u32(&o, 0); /* создан */
	u32(&o, 0); /* изменён */
	u32(&o, 1000);
	u32(&o, ms);
	u32(&o, 0x10000); /* скорость 1.0 */
	u16(&o, 0x100);   /* громкость 1.0 */
	zeros(&o, 10);
	matrix(&o);
	zeros(&o, 24);
	u32(&o, 2); /* следующий track_ID */

	box(&o, TRAK(n), "trak");
	fbox(&o, TKHD, "tkhd", 7); /* включена, в фильме, в превью */
	u32(&o, 0);
	u32(&o, 0);
	u32(&o, 1); /* track_ID */
	u32(&o, 0);
	u32(&o, ms);
	zeros(&o, 8);
	u16(&o, 0); /* слой */
	u16(&o, 0); /* группа */
	u16(&o, 0x100);
	u16(&o, 0);
	matrix(&o);
	u32(&o, 0); /* ширина, высота — у звука 0 */
	u32(&o, 0);

	box(&o, MDIA(n), "mdia");
	fbox(&o, MDHD, "mdhd", 0);
	u32(&o, 0);
	u32(&o, 0);
	u32(&o, info->sample_rate);
	u32(&o, samples);
	u16(&o, 0x55c4); /* язык "und" */
	u16(&o, 0);

	fbox(&o, HDLR, "hdlr", 0);
	u32(&o, 0);
	fourcc(&o, "soun");
	zeros(&o, 12);
	for (const char *s = "SoundHandler"; ; s++) {
		u8(&o, *s);
		if (!*s) {
			break;
		}
	}

	box(&o, MINF(n), "minf");
	fbox(&o, SMHD, "smhd", 0);
	u32(&o, 0); /* баланс, резерв */
	box(&o, DINF, "dinf");
	fbox(&o, DINF - 8, "dref", 0);
	u32(&o, 1);
	fbox(&o, 12, "url ", 1); /* данные в этом же файле */

	box(&o, STBL(n), "stbl");
	fbox(&o, STSD, "stsd", 0);
	u32(&o, 1);
	box(&o, MP4A, "mp4a");
	zeros(&o, 6);
	u16(&o, 1); /* data_reference_index */
	zeros(&o, 8);
	u16(&o, info->channels);
	u16(&o, 16);
	zeros(&o, 4);
	u32(&o, info->sample_rate << 16);

	fbox(&o, ESDS, "esds", 0);
	u8(&o, 0x03); /* ES_Descriptor */
	u8(&o, 25);
	u16(&o, 0);   /* ES_ID */
	u8(&o, 0);
	u8(&o, 0x04); /* DecoderConfigDescriptor */
	u8(&o, 17);
	u8(&o, 0x40); /* MPEG-4 Audio */
	u8(&o, 0x15); /* поток звука */
	u8(&o, 0);    /* bufferSizeDB, 3 байта */
	u16(&o, 0);
	u32(&o, info->bitrate); /* max */
	u32(&o, info->bitrate); /* avg */
	u8(&o, 0x05);           /* AudioSpecificConfig: AAC-LC, частота, каналы */
	u8(&o, 2);
	u8(&o, (2 << 3) | (idx >> 1));
	u8(&o, (idx << 7) | (info->channels << 3));
	u8(&o, 0x06); /* SLConfigDescriptor */
	u8(&o, 1);
	u8(&o, 0x02);

	fbox(&o, STTS, "stts", 0);
	u32(&o, 1);
	u32(&o, n);
	u32(&o, FRAME_SAMPLES);

	/* все кадры — один кусок подряд в mdat */
	fbox(&o, STSC, "stsc", 0);
	u32(&o, 1);
	u32(&o, 1);
	u32(&o, n);
	u32(&o, 1);

	fbox(&o, STSZ(n), "stsz", 0);
	u32(&o, 0); /* размеры разные — таблица */
	u32(&o, n);
	for (uint32_t i = 0; i < n; i++) {
		u32(&o, next_size(size_ctx));
	}

	fbox(&o, STCO, "stco", 0);
	u32(&o, 1);
	u32(&o, M4A_HEAD_SIZE);

	flush(&o);
}
