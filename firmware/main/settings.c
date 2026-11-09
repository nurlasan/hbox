#include "settings.h"

#include <string.h>
#include <nvs.h>
#include <nvs_flash.h>

#define NS "hbox"

void settings_load(struct settings *s)
{
	nvs_handle_t h;
	size_t len;
	int32_t v;
	uint8_t b;

	*s = (struct settings){.format = "mp3", .bitrate = 128000, .record = true};
	if (nvs_flash_init() != ESP_OK) {
		nvs_flash_erase();
		nvs_flash_init();
	}
	if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
		return;
	}
	len = sizeof(s->token);
	nvs_get_str(h, "token", s->token, &len);
	len = sizeof(s->channel);
	nvs_get_str(h, "channel", s->channel, &len);
	len = sizeof(s->format);
	nvs_get_str(h, "format", s->format, &len);
	if (nvs_get_i32(h, "bitrate", &v) == ESP_OK) {
		s->bitrate = v;
	}
	if (nvs_get_u8(h, "record", &b) == ESP_OK) {
		s->record = b;
	}
	nvs_close(h);
}

void settings_save(const struct settings *s)
{
	nvs_handle_t h;

	if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
		return;
	}
	nvs_set_str(h, "token", s->token);
	nvs_set_str(h, "channel", s->channel);
	nvs_set_str(h, "format", s->format);
	nvs_set_i32(h, "bitrate", s->bitrate);
	nvs_set_u8(h, "record", s->record);
	nvs_commit(h);
	nvs_close(h);
}
