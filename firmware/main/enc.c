#include "enc.h"

#include <string.h>
#include <esp_aac_enc.h>
#include <esp_heap_caps.h>
#include <layer3.h>

#include "m4a.h"
#include "mic.h"

/* кадров AAC в файле с запасом: 1024 отсчёта на кадр */
#define M4A_FRAMES_MAX (STORE_FILE_SECONDS * MIC_SAMPLE_RATE / 1024 + 64)

static struct store_file *file;
static bool m4a;
static bool ok;

static shine_t mp3;

static void *aac;
static uint8_t *aac_out;
static int aac_in_size, aac_out_size;
static struct m4a_info info;
/* размеры кадров для moov: до 64 КБ на кадр не бывает, хватает uint16 */
static uint16_t *sizes;
static uint32_t sizes_next;

const char *enc_ext(const char *format)
{
	return format && strcmp(format, "m4a") == 0 ? "m4a" : "mp3";
}

static size_t mp3_open(int bitrate)
{
	shine_config_t cfg;

	shine_set_config_mpeg_defaults(&cfg.mpeg);
	cfg.wave.channels = PCM_MONO;
	cfg.wave.samplerate = MIC_SAMPLE_RATE;
	cfg.mpeg.mode = MONO;
	cfg.mpeg.bitr = bitrate / 1000;
	if (shine_check_config(cfg.wave.samplerate, cfg.mpeg.bitr) < 0) {
		cfg.mpeg.bitr = 128;
	}
	mp3 = shine_initialise(&cfg);
	return mp3 ? shine_samples_per_pass(mp3) : 0;
}

static size_t aac_open(int bitrate)
{
	esp_aac_enc_config_t cfg = ESP_AAC_ENC_CONFIG_DEFAULT();
	uint8_t head[M4A_HEAD_SIZE];

	cfg.sample_rate = MIC_SAMPLE_RATE;
	cfg.channel = ESP_AUDIO_MONO;
	cfg.bitrate = bitrate;
	cfg.adts_used = false; /* кадры кладём в .m4a */
	if (!sizes) {
		sizes = heap_caps_malloc(M4A_FRAMES_MAX * sizeof(sizes[0]), MALLOC_CAP_SPIRAM);
	}
	if (!sizes || esp_aac_enc_open(&cfg, sizeof(cfg), &aac) != ESP_AUDIO_ERR_OK) {
		return 0;
	}
	esp_aac_enc_get_frame_size(aac, &aac_in_size, &aac_out_size);
	aac_out = heap_caps_aligned_alloc(16, aac_out_size, MALLOC_CAP_INTERNAL);
	if (!aac_out) {
		esp_aac_enc_close(aac);
		return 0;
	}
	info = (struct m4a_info){.sample_rate = MIC_SAMPLE_RATE, .channels = 1, .bitrate = bitrate};
	m4a_head(head);
	store_write(file, head, sizeof(head));
	return aac_in_size / sizeof(int16_t);
}

size_t enc_start(struct store_file *f, const char *ext, int bitrate)
{
	file = f;
	m4a = strcmp(ext, "m4a") == 0;
	ok = true;
	return m4a ? aac_open(bitrate) : mp3_open(bitrate);
}

static bool put(const void *data, size_t len)
{
	ok = ok && store_write(file, data, len);
	return ok;
}

bool enc_frame(int16_t *pcm)
{
	if (!m4a) {
		int n;
		unsigned char *out = shine_encode_buffer(mp3, &pcm, &n);

		return put(out, n);
	}

	esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = aac_in_size};
	esp_audio_enc_out_frame_t out = {.buffer = aac_out, .len = aac_out_size};

	if (esp_aac_enc_process(aac, &in, &out) != ESP_AUDIO_ERR_OK) {
		return ok = false;
	}
	if (!out.encoded_bytes) {
		return ok;
	}
	/* в moov — только кадры, которые легли в файл */
	if (info.frames == M4A_FRAMES_MAX || !put(aac_out, out.encoded_bytes)) {
		return ok = false;
	}
	sizes[info.frames++] = out.encoded_bytes;
	info.data_bytes += out.encoded_bytes;
	return true;
}

static uint32_t next_size(void *ctx)
{
	return sizes[sizes_next++];
}

static void put_cb(void *ctx, const void *data, size_t len)
{
	put(data, len);
}

void enc_finish(void)
{
	if (!m4a) {
		int n;
		unsigned char *out = shine_flush(mp3, &n);

		put(out, n);
		shine_close(mp3);
		return;
	}
	esp_aac_enc_close(aac);
	heap_caps_free(aac_out);

	uint32_t mdat = m4a_mdat_size(&info);
	uint8_t be[4] = {mdat >> 24, mdat >> 16, mdat >> 8, mdat};

	sizes_next = 0;
	m4a_moov(&info, next_size, NULL, put_cb, NULL);
	store_pwrite(file, M4A_MDAT_SIZE_OFFSET, be, sizeof(be));
}
