#ifndef AUDIO_CACHE_SERVICE_H
#define AUDIO_CACHE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_CACHE_PATH_MAX 160
#define AUDIO_CACHE_URL_SIZE 256

typedef struct {
    char audio_url[AUDIO_CACHE_URL_SIZE];
    int64_t ring_at_epoch;
} audio_cache_maintenance_item_t;

typedef void (*audio_cache_maintenance_result_cb_t)(const char *audio_url,
                                                    esp_err_t ret,
                                                    const char *local_path,
                                                    void *ctx);

esp_err_t audio_cache_service_init(void);
bool audio_cache_service_is_ready(void);
bool audio_cache_service_file_exists(const char *path);
uint32_t audio_cache_service_hash_url(const char *audio_url);
esp_err_t audio_cache_service_resolve_path(const char *instance_id,
                                           const char *audio_url,
                                           char *path_buffer,
                                           size_t path_buffer_size);
bool audio_cache_service_find_existing(const char *instance_id,
                                       const char *audio_url,
                                       char *path_buffer,
                                       size_t path_buffer_size);
esp_err_t audio_cache_service_download(const char *instance_id,
                                       const char *audio_url,
                                       char *path_buffer,
                                       size_t path_buffer_size);
esp_err_t audio_cache_service_cleanup_unused(const char *const *keep_paths, size_t keep_path_count);
esp_err_t audio_cache_service_cleanup_unused_protected(const char *const *keep_paths,
                                                       size_t keep_path_count,
                                                       const char *protected_path);
esp_err_t audio_cache_service_maintain(const audio_cache_maintenance_item_t *items,
                                       size_t item_count,
                                       const char *protected_path,
                                       audio_cache_maintenance_result_cb_t result_cb,
                                       void *result_ctx);

#endif
