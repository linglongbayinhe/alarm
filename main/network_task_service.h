#ifndef NETWORK_TASK_SERVICE_H
#define NETWORK_TASK_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "audio_cache_service.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

typedef void (*network_task_json_result_cb_t)(esp_err_t ret,
                                              const char *json,
                                              size_t json_len,
                                              void *ctx);
typedef void (*network_task_audio_cache_result_cb_t)(const char *download_url,
                                                     esp_err_t ret,
                                                     const char *local_path,
                                                     void *ctx);
typedef void (*network_task_audio_cache_done_cb_t)(esp_err_t ret, void *ctx);
typedef void (*network_task_startup_pull_done_cb_t)(esp_err_t ret, void *ctx);
typedef void (*network_task_startup_weather_done_cb_t)(esp_err_t ret, void *ctx);

typedef enum {
    NETWORK_TASK_PLAYBACK_REASON_NONE = 0,
    NETWORK_TASK_PLAYBACK_REASON_NORMAL,
    NETWORK_TASK_PLAYBACK_REASON_STARTUP,
    NETWORK_TASK_PLAYBACK_REASON_POST_PLAYBACK,
} network_task_playback_reason_t;

typedef enum {
    NETWORK_TASK_WEATHER_REASON_NONE = 0,
    NETWORK_TASK_WEATHER_REASON_NORMAL,
    NETWORK_TASK_WEATHER_REASON_STARTUP,
} network_task_weather_reason_t;

typedef struct {
    char download_url[AUDIO_CACHE_URL_SIZE];
    int64_t ring_at_epoch;
} network_task_audio_cache_item_t;

esp_err_t network_task_service_init(void);
esp_err_t network_task_service_start(EventGroupHandle_t connected_event_group, EventBits_t connected_bit);

void network_task_service_register_playback_pull_handler(network_task_json_result_cb_t handler, void *ctx);
void network_task_service_register_weather_handler(network_task_json_result_cb_t handler, void *ctx);
void network_task_service_register_audio_cache_handler(network_task_audio_cache_result_cb_t handler, void *ctx);
void network_task_service_register_audio_cache_done_handler(network_task_audio_cache_done_cb_t handler, void *ctx);
void network_task_service_register_startup_pull_done_handler(network_task_startup_pull_done_cb_t handler, void *ctx);
void network_task_service_register_startup_weather_done_handler(network_task_startup_weather_done_cb_t handler,
                                                                void *ctx);

void network_task_service_request_playback_pull(network_task_playback_reason_t reason);
void network_task_service_request_weather_refresh(network_task_weather_reason_t reason);
void network_task_service_request_audio_cache_maintenance(const network_task_audio_cache_item_t *items,
                                                          size_t item_count,
                                                          const char *protected_path);
void network_task_service_request_playback_report(const char *instance_id,
                                                  const char *status,
                                                  const char *audio_status);
void network_task_service_reset_sessions(void);

#endif
