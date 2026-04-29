#ifndef NETWORK_TASK_SERVICE_H
#define NETWORK_TASK_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

typedef void (*network_task_json_result_cb_t)(esp_err_t ret,
                                              const char *json,
                                              size_t json_len,
                                              void *ctx);
typedef void (*network_task_audio_result_cb_t)(const char *instance_id,
                                               esp_err_t ret,
                                               const char *local_path,
                                               void *ctx);

esp_err_t network_task_service_init(void);
esp_err_t network_task_service_start(EventGroupHandle_t connected_event_group, EventBits_t connected_bit);

void network_task_service_register_playback_pull_handler(network_task_json_result_cb_t handler, void *ctx);
void network_task_service_register_weather_handler(network_task_json_result_cb_t handler, void *ctx);
void network_task_service_register_audio_download_handler(network_task_audio_result_cb_t handler, void *ctx);

void network_task_service_request_playback_pull(bool post_playback);
void network_task_service_request_weather_refresh(void);
void network_task_service_request_audio_download(const char *instance_id, const char *audio_url);
void network_task_service_request_playback_report(const char *instance_id,
                                                  const char *status,
                                                  const char *audio_status);
void network_task_service_reset_sessions(void);

#endif
