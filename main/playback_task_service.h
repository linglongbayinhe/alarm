#ifndef PLAYBACK_TASK_SERVICE_H
#define PLAYBACK_TASK_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define PLAYBACK_TASK_MAX_COUNT 24
#define PLAYBACK_TASK_ID_SIZE 48
#define PLAYBACK_TASK_TITLE_SIZE 64
#define PLAYBACK_TASK_AUDIO_URL_SIZE 256
#define PLAYBACK_TASK_FALLBACK_MODE_SIZE 24
#define PLAYBACK_TASK_LOCAL_PATH_SIZE 96

typedef enum {
    PLAYBACK_TASK_STATUS_PENDING = 0,
    PLAYBACK_TASK_STATUS_READY = 1,
    PLAYBACK_TASK_STATUS_PLAYING = 2,
    PLAYBACK_TASK_STATUS_FINISHED = 3,
    PLAYBACK_TASK_STATUS_FAILED = 4,
    PLAYBACK_TASK_STATUS_REPORTED = 255,
} playback_task_status_t;

typedef enum {
    PLAYBACK_AUDIO_STATUS_NONE = 0,
    PLAYBACK_AUDIO_STATUS_WAITING = 1,
    PLAYBACK_AUDIO_STATUS_CACHED = 2,
    PLAYBACK_AUDIO_STATUS_FAILED = 3,
} playback_audio_status_t;

typedef struct {
    char instance_id[PLAYBACK_TASK_ID_SIZE];
    char alarm_id[PLAYBACK_TASK_ID_SIZE];
    int64_t ring_at_epoch;
    char title[PLAYBACK_TASK_TITLE_SIZE];
    char audio_url[PLAYBACK_TASK_AUDIO_URL_SIZE];
    char fallback_mode[PLAYBACK_TASK_FALLBACK_MODE_SIZE];
    uint8_t task_status;
    uint8_t audio_status;
    char local_audio_path[PLAYBACK_TASK_LOCAL_PATH_SIZE];
    uint8_t audio_cached;
    uint8_t last_reported_status;
    uint8_t retry_count;
    int64_t expires_at_epoch;
} playback_task_t;

esp_err_t playback_task_service_init(void);
esp_err_t playback_task_service_start(EventGroupHandle_t connected_event_group, EventBits_t connected_bit);
void playback_task_service_request_sync(void);

#endif
