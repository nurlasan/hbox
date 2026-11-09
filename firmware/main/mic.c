#include "mic.h"

#include <math.h>
#include <driver/i2s_std.h>

#include "board.h"

#define CHUNK 256

static i2s_chan_handle_t rx;
#define GAIN 10.0f /* +20 дБ, как в music-monitor */
static volatile uint32_t overflows;

/*
 * ФВЧ 40 Гц, два звена первого порядка: у INMP441 сильный дрейф ниже 5 Гц (на SuperMini до
 * -12 дБFS при тишине), без фильтра он с усилением +20 дБ уходит в срез.
 */
#define HPF_A (1.0f / (1.0f + 2.0f * (float)M_PI * 40.0f / MIC_SAMPLE_RATE))

static float hpf_x[2], hpf_y[2];

static float hpf_step(float x)
{
	for (int i = 0; i < 2; i++) {
		hpf_y[i] = HPF_A * (hpf_y[i] + x - hpf_x[i]);
		hpf_x[i] = x;
		x = hpf_y[i];
	}
	return x;
}

static int16_t sat16(float x)
{
	return x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int16_t)lrintf(x);
}

static bool IRAM_ATTR on_overflow(i2s_chan_handle_t h, i2s_event_data_t *e, void *ctx)
{
	overflows++;
	return false;
}

esp_err_t mic_start(void)
{
	i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	/* 8 × 1023 отсчётов ≈ 185 мс запаса, пока кодировщик занят кадром */
	chan.dma_desc_num = 8;
	chan.dma_frame_num = 1023;
	i2s_std_config_t std = {
		.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
								I2S_SLOT_MODE_MONO),
		.gpio_cfg = {
			.mclk = I2S_GPIO_UNUSED,
			.bclk = MIC_PIN_SCK,
			.ws = MIC_PIN_WS,
			.dout = I2S_GPIO_UNUSED,
			.din = MIC_PIN_SD,
		},
	};
	i2s_event_callbacks_t cbs = {.on_recv_q_ovf = on_overflow};
	esp_err_t err;

	std.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
	err = i2s_new_channel(&chan, NULL, &rx);
	if (!err) {
		err = i2s_channel_init_std_mode(rx, &std);
	}
	if (!err) {
		err = i2s_channel_register_event_callback(rx, &cbs, NULL);
	}
	return err ? err : i2s_channel_enable(rx);
}

void mic_stop(void)
{
	i2s_channel_disable(rx);
	i2s_del_channel(rx);
	rx = NULL;
}

/* INMP441: 24 бита в старших разрядах 32-битного слота; >> 16 — те же 0 дБ, что int16. */
esp_err_t mic_read(int16_t *out, size_t n)
{
	static int32_t raw[CHUNK];

	while (n) {
		size_t want = n < CHUNK ? n : CHUNK, got;
		esp_err_t err = i2s_channel_read(rx, raw, want * sizeof(raw[0]), &got, 1000);

		if (err) {
			return err;
		}
		got /= sizeof(raw[0]);
		for (size_t i = 0; i < got; i++) {
			*out++ = sat16(hpf_step((float)(raw[i] >> 8) / 256.0f) * GAIN);
		}
		n -= got;
	}
	return ESP_OK;
}

uint32_t mic_overflows(void)
{
	return overflows;
}
