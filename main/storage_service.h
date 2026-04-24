#ifndef STORAGE_SERVICE_H
#define STORAGE_SERVICE_H

#include <stdbool.h>

#include "esp_err.h"

#define STORAGE_SERVICE_INTERNAL_BASE_PATH "/spiffs"
#define STORAGE_SERVICE_DEFAULT_AUDIO_PATH "/spiffs/audio/default_alarm.wav"
#define STORAGE_SERVICE_EXTERNAL_BASE_PATH "/ext"
#define STORAGE_SERVICE_EXTERNAL_AUDIO_DIR "/ext/audio"

esp_err_t storage_service_init(void);
bool storage_service_default_audio_exists(void);
const char *storage_service_get_default_audio_path(void);
bool storage_service_external_cache_available(void);

#endif
