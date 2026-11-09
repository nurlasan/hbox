#include "bench.h"

#include <stdio.h>
#include <string.h>
#include <esp_aac_enc.h>
#include <esp_heap_caps.h>
#include <esp_rom_crc.h>
#include <esp_timer.h>
#include <layer3.h>

#include "mic.h"

#define BITRATE 128000
#define SECONDS 10
#define OUT_MAX (BITRATE / 8 * SECONDS + 16384)

struct codec {
	const char *name;
	const char *ext;
	/* отсчётов на кадр; 0 — не открылся */
	size_t (*open)(void);
	/* кодирует кадр, возвращает число байт в *out (может быть 0 — кодировщик копит) */
	int (*encode)(int16_t *pcm, const uint8_t **out);
	void (*close)(void);
};

static shine_t mp3;

static size_t mp3_open(void)
{
	shine_config_t cfg;

	shine_set_config_mpeg_defaults(&cfg.mpeg);
	cfg.wave.channels = PCM_MONO;
	cfg.wave.samplerate = MIC_SAMPLE_RATE;
	cfg.mpeg.mode = MONO;
	cfg.mpeg.bitr = BITRATE / 1000;
	mp3 = shine_initialise(&cfg);
	return mp3 ? shine_samples_per_pass(mp3) : 0;
}

static int mp3_encode(int16_t *pcm, const uint8_t **out)
{
	int written;

	*out = shine_encode_buffer(mp3, &pcm, &written);
	return written;
}

static void mp3_close(void)
{
	shine_close(mp3);
}

static void *aac;
static uint8_t *aac_out;
static int aac_in_size, aac_out_size;

static size_t aac_open(void)
{
	esp_aac_enc_config_t cfg = ESP_AAC_ENC_CONFIG_DEFAULT();

	cfg.sample_rate = MIC_SAMPLE_RATE;
	cfg.channel = ESP_AUDIO_MONO;
	cfg.bitrate = BITRATE;
	if (esp_aac_enc_open(&cfg, sizeof(cfg), &aac) != ESP_AUDIO_ERR_OK) {
		return 0;
	}
	esp_aac_enc_get_frame_size(aac, &aac_in_size, &aac_out_size);
	aac_out = heap_caps_aligned_alloc(16, aac_out_size, MALLOC_CAP_INTERNAL);
	return aac_out ? aac_in_size / sizeof(int16_t) : 0;
}

static int aac_encode(int16_t *pcm, const uint8_t **out)
{
	esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = aac_in_size};
	esp_audio_enc_out_frame_t o = {.buffer = aac_out, .len = aac_out_size};

	if (esp_aac_enc_process(aac, &in, &o) != ESP_AUDIO_ERR_OK) {
		return -1;
	}
	*out = aac_out;
	return o.encoded_bytes;
}

static void aac_close(void)
{
	esp_aac_enc_close(aac);
	heap_caps_free(aac_out);
}

static const struct codec codecs[] = {
	{"mp3", "mp3", mp3_open, mp3_encode, mp3_close},
	{"aac", "aac", aac_open, aac_encode, aac_close},
};

static void dump(const char *name, const uint8_t *data, size_t len)
{
	static const char b64[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	char line[77];

	printf("HBOX FILE %s %u\n", name, (unsigned)len);
	for (size_t off = 0; off < len; off += 57) {
		size_t n = len - off < 57 ? len - off : 57, k = 0;

		for (size_t i = 0; i < n; i += 3) {
			uint32_t v = data[off + i] << 16;

			v |= i + 1 < n ? data[off + i + 1] << 8 : 0;
			v |= i + 2 < n ? data[off + i + 2] : 0;
			line[k++] = b64[v >> 18];
			line[k++] = b64[(v >> 12) & 63];
			line[k++] = i + 1 < n ? b64[(v >> 6) & 63] : '=';
			line[k++] = i + 2 < n ? b64[v & 63] : '=';
		}
		line[k] = 0;
		puts(line);
	}
	printf("HBOX END %08lx\n", (unsigned long)esp_rom_crc32_le(0, data, len));
	fflush(stdout);
}

static void run(const struct codec *c, uint8_t *buf, size_t cap)
{
	size_t frame = c->open(), len = 0, frames = 0, total;
	int64_t busy = 0, worst = 0, t0;
	uint32_t ovf;
	int16_t *pcm;

	if (!frame) {
		printf("HBOX BENCH codec=%s ERROR open\n", c->name);
		return;
	}
	pcm = heap_caps_aligned_alloc(16, frame * sizeof(int16_t), MALLOC_CAP_INTERNAL);
	total = (size_t)SECONDS * MIC_SAMPLE_RATE / frame;
	ovf = mic_overflows();
	for (; pcm && frames < total; frames++) {
		const uint8_t *out;
		int n;

		if (mic_read(pcm, frame) != ESP_OK) {
			printf("HBOX BENCH codec=%s ERROR mic\n", c->name);
			break;
		}
		t0 = esp_timer_get_time();
		n = c->encode(pcm, &out);
		t0 = esp_timer_get_time() - t0;
		busy += t0;
		worst = t0 > worst ? t0 : worst;
		if (n < 0) {
			printf("HBOX BENCH codec=%s ERROR encode\n", c->name);
			break;
		}
		if (len + n <= cap) {
			memcpy(buf + len, out, n);
			len += n;
		}
	}
	ovf = mic_overflows() - ovf;
	c->close();
	heap_caps_free(pcm);

	/* доля времени ядра на кодирование: кадр звука длится frame / 44100 с */
	double audio_us = (double)frames * frame * 1e6 / MIC_SAMPLE_RATE;
	printf("HBOX BENCH codec=%s rate=%d kbps=%d sec=%.1f cpu=%.1f%% worst_ms=%.1f "
	       "frame_ms=%.1f bytes=%u kbps_real=%.0f overflows=%lu heap_min=%u\n",
	       c->name, MIC_SAMPLE_RATE, BITRATE / 1000, audio_us / 1e6,
	       audio_us ? busy * 100.0 / audio_us : 0, worst / 1000.0,
	       frame * 1000.0 / MIC_SAMPLE_RATE, (unsigned)len,
	       audio_us ? len * 8 * 1000.0 / audio_us : 0, (unsigned long)ovf,
	       (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));

	char name[16];

	snprintf(name, sizeof(name), "bench.%s", c->ext);
	dump(name, buf, len);
}

void bench_run(void)
{
	uint8_t *buf = heap_caps_malloc(OUT_MAX, MALLOC_CAP_SPIRAM);
	size_t cap = OUT_MAX;

	/* без PSRAM — сколько влезет во внутреннюю память; запись короче, замер CPU тот же */
	if (!buf) {
		cap = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) - 96 * 1024;
		cap = cap > OUT_MAX ? OUT_MAX : cap;
		buf = heap_caps_malloc(cap, MALLOC_CAP_INTERNAL);
	}
	if (!buf) {
		printf("HBOX BENCH ERROR no memory\n");
		return;
	}
	printf("HBOX BENCH start psram=%s buf=%u\n",
	       heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? "yes" : "no", (unsigned)cap);
	for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); i++) {
		run(&codecs[i], buf, cap);
	}
	heap_caps_free(buf);
}
