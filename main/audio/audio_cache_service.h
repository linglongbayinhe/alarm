#ifndef AUDIO_CACHE_SERVICE_H
#define AUDIO_CACHE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t audio_cache_service_init(void);
bool audio_cache_service_is_ready(void);
bool audio_cache_service_file_exists(const char *path);
esp_err_t audio_cache_service_resolve_path(const char *instance_id,
                                           const char *audio_url,
                                           char *path_buffer,
                                           size_t path_buffer_size);
esp_err_t audio_cache_service_download(const char *instance_id,
                                       const char *audio_url,
                                       char *path_buffer,
                                       size_t path_buffer_size);
esp_err_t audio_cache_service_cleanup_unused(const char *const *keep_paths, size_t keep_path_count);

#endif
