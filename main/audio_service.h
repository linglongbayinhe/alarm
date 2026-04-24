#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AUDIO_SERVICE_FORMAT_AUTO = 0,
    AUDIO_SERVICE_FORMAT_WAV = 1,
    AUDIO_SERVICE_FORMAT_MP3 = 2,
} audio_service_format_t;

esp_err_t audio_service_init(void);
esp_err_t audio_service_play(const char *path,
                             audio_service_format_t format,
                             uint8_t volume_percent,
                             uint32_t max_duration_ms);
bool audio_service_is_ready(void);

#endif
