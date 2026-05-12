#include "playback_task_service.h"

#include <ctype.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_cache_service.h"
#include "audio_service.h"
#include "cJSON.h"
#include "device_cloud_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "network_task_service.h"
#include "nvs.h"
#include "storage_service.h"
#include "time_service.h"

static const char *TAG = "PLAYBACK_TASK";
static const char *PLAYBACK_TASK_NAMESPACE = "playback";
static const char *PLAYBACK_TASK_BLOB_KEY = "tasks_v1";

#define PLAYBACK_TASK_TASK_STACK_SIZE             6144
#define PLAYBACK_TASK_TASK_PRIORITY               4
#define PLAYBACK_TASK_HTTP_GRACE_SECONDS          300
#define PLAYBACK_TASK_KEEP_FILE_SECONDS           (24 * 60 * 60)
#define PLAYBACK_TASK_NOTIFY_SYNC                 BIT0
#define PLAYBACK_TASK_NOTIFY_SAVE                 BIT1
#define PLAYBACK_TASK_NOTIFY_PULL_RESULT          BIT2
#define PLAYBACK_TASK_NOTIFY_AUDIO_RESULT         BIT3
#define PLAYBACK_TASK_TIME_WAIT_MS                5000
#define PLAYBACK_TASK_SAVE_DELAY_MS               3000
#define PLAYBACK_TASK_DEFAULT_VOLUME_PERCENT      50

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t task_count;
    playback_task_t tasks[PLAYBACK_TASK_MAX_COUNT];
} playback_task_blob_t;

typedef struct {
    bool in_use;
    char local_path[AUDIO_CACHE_PATH_MAX];
    esp_err_t ret;
} playback_audio_result_t;

static playback_task_t s_tasks[PLAYBACK_TASK_MAX_COUNT];
static size_t s_task_count;
static EventGroupHandle_t s_connected_event_group;
static EventBits_t s_connected_bit;
static TaskHandle_t s_task_handle;
static TimerHandle_t s_save_timer;
static bool s_force_sync = true;
static bool s_state_dirty;
static uint32_t s_last_config_generation;
static uint8_t s_current_volume_percent = PLAYBACK_TASK_DEFAULT_VOLUME_PERCENT;
static portMUX_TYPE s_pull_result_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_pull_result_pending;
static esp_err_t s_pull_result_ret = ESP_OK;
static const char *s_pull_result_json;
static bool s_startup_pull_result_seen;
static SemaphoreHandle_t s_pull_result_done_sem;
static portMUX_TYPE s_audio_result_lock = portMUX_INITIALIZER_UNLOCKED;
static playback_audio_result_t s_audio_result;
static SemaphoreHandle_t s_audio_result_done_sem;
static char s_current_playing_path[AUDIO_CACHE_PATH_MAX];

static void playback_task_mark_dirty(bool immediate);
static void playback_task_flush_state_if_dirty(void);
static void playback_task_log_next_due(time_t now);

static size_t playback_task_blob_size_for_count(size_t task_count)
{
    if (task_count > PLAYBACK_TASK_MAX_COUNT) {
        task_count = PLAYBACK_TASK_MAX_COUNT;
    }

    return offsetof(playback_task_blob_t, tasks) + (sizeof(playback_task_t) * task_count);
}

static void playback_task_copy_string(char *destination, size_t destination_size, const char *source)
{
    if ((destination == NULL) || (destination_size == 0)) {
        return;
    }

    snprintf(destination, destination_size, "%s", source == NULL ? "" : source);
}

static bool playback_task_string_equals_ignore_case(const char *left, const char *right)
{
    while ((left != NULL) && (right != NULL) && (*left != '\0') && (*right != '\0')) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }

    return (left != NULL) && (right != NULL) && (*left == '\0') && (*right == '\0');
}

static int playback_task_compare_ring_time(const void *left, const void *right)
{
    const playback_task_t *left_task = (const playback_task_t *)left;
    const playback_task_t *right_task = (const playback_task_t *)right;

    if (left_task->ring_at_epoch < right_task->ring_at_epoch) {
        return -1;
    }
    if (left_task->ring_at_epoch > right_task->ring_at_epoch) {
        return 1;
    }

    return strcmp(left_task->instance_id, right_task->instance_id);
}

static esp_err_t playback_task_save_state_to_nvs(void)
{
    playback_task_blob_t *blob = NULL;
    nvs_handle_t handle = 0;
    size_t blob_size = playback_task_blob_size_for_count(s_task_count);
    esp_err_t ret = ESP_OK;

    blob = calloc(1, blob_size);
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    blob->magic = 0x50544231U;
    blob->version = 3;
    blob->task_count = (uint32_t)s_task_count;
    if (s_task_count > 0) {
        memcpy(blob->tasks, s_tasks, sizeof(playback_task_t) * s_task_count);
    }

    ret = nvs_open(PLAYBACK_TASK_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        free(blob);
        return ret;
    }

    ret = nvs_set_blob(handle, PLAYBACK_TASK_BLOB_KEY, blob, blob_size);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    free(blob);
    return ret;
}

static bool playback_task_remove_expired(time_t now)
{
    size_t write_index = 0;
    size_t read_index = 0;
    int64_t stale_due_epoch = (int64_t)now - PLAYBACK_TASK_HTTP_GRACE_SECONDS;
    bool changed = false;

    for (read_index = 0; read_index < s_task_count; ++read_index) {
        const playback_task_t *task = &s_tasks[read_index];
        if ((task->expires_at_epoch > 0) && (task->expires_at_epoch < now)) {
            changed = true;
            continue;
        }
        if ((task->ring_at_epoch > 0) &&
            (task->ring_at_epoch < stale_due_epoch) &&
            (task->task_status != PLAYBACK_TASK_STATUS_FINISHED) &&
            (task->task_status != PLAYBACK_TASK_STATUS_FAILED)) {
            ESP_LOGI(TAG,
                     "Dropping stale playback task %s ring_at=%" PRId64,
                     task->instance_id,
                     task->ring_at_epoch);
            changed = true;
            continue;
        }
        if (write_index != read_index) {
            s_tasks[write_index] = s_tasks[read_index];
            changed = true;
        }
        ++write_index;
    }

    if (write_index != s_task_count) {
        s_task_count = write_index;
        changed = true;
    }

    return changed;
}

static void playback_task_load_state(void)
{
    playback_task_blob_t *blob = NULL;
    nvs_handle_t handle = 0;
    size_t blob_size = sizeof(playback_task_blob_t);

    blob = calloc(1, sizeof(*blob));
    if (blob == NULL) {
        return;
    }

    if (nvs_open(PLAYBACK_TASK_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        free(blob);
        return;
    }

    if (nvs_get_blob(handle, PLAYBACK_TASK_BLOB_KEY, blob, &blob_size) == ESP_OK) {
        if ((blob->magic == 0x50544231U) &&
            (blob->version == 3U) &&
            (blob->task_count <= PLAYBACK_TASK_MAX_COUNT) &&
            (blob_size >= playback_task_blob_size_for_count(blob->task_count))) {
            s_task_count = blob->task_count;
            if (s_task_count > 0) {
                memcpy(s_tasks, blob->tasks, s_task_count * sizeof(playback_task_t));
            }
            playback_task_remove_expired(time(NULL));
        }
    }

    nvs_close(handle);
    free(blob);
}

static playback_task_t *playback_task_find_by_instance(const char *instance_id)
{
    size_t index = 0;

    for (index = 0; index < s_task_count; ++index) {
        if (strcmp(s_tasks[index].instance_id, instance_id) == 0) {
            return &s_tasks[index];
        }
    }

    return NULL;
}

static bool playback_task_allows_fallback(const playback_task_t *task)
{
    if (task == NULL) {
        return true;
    }
    if ((task->fallback_mode[0] == '\0') ||
        playback_task_string_equals_ignore_case(task->fallback_mode, "default") ||
        playback_task_string_equals_ignore_case(task->fallback_mode, "fallback")) {
        return true;
    }
    if (playback_task_string_equals_ignore_case(task->fallback_mode, "none") ||
        playback_task_string_equals_ignore_case(task->fallback_mode, "off") ||
        playback_task_string_equals_ignore_case(task->fallback_mode, "disable")) {
        return false;
    }

    return true;
}

static bool playback_task_is_connected(void)
{
    if (s_connected_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_connected_event_group) & s_connected_bit) != 0;
}

static bool playback_task_parse_epoch_from_string(const char *value, int64_t *epoch_out)
{
    struct tm time_info = {0};
    const char *formats[] = {
        "%Y-%m-%d %H:%M:%S",
        "%Y-%m-%dT%H:%M:%S",
        "%Y/%m/%d %H:%M:%S",
    };
    size_t index = 0;

    if ((value == NULL) || (epoch_out == NULL)) {
        return false;
    }

    if (isdigit((unsigned char)value[0])) {
        char *endptr = NULL;
        int64_t numeric = strtoll(value, &endptr, 10);
        if ((endptr != NULL) && (*endptr == '\0')) {
            *epoch_out = numeric > 20000000000LL ? (numeric / 1000LL) : numeric;
            return true;
        }
    }

    for (index = 0; index < (sizeof(formats) / sizeof(formats[0])); ++index) {
        memset(&time_info, 0, sizeof(time_info));
        if (strptime(value, formats[index], &time_info) != NULL) {
            time_info.tm_isdst = -1;
            *epoch_out = (int64_t)mktime(&time_info);
            return *epoch_out > 0;
        }
    }

    return false;
}

static bool playback_task_parse_ring_at(const cJSON *item, int64_t *epoch_out)
{
    if ((item == NULL) || (epoch_out == NULL)) {
        return false;
    }
    if (cJSON_IsNumber(item)) {
        int64_t value = (int64_t)item->valuedouble;
        *epoch_out = value > 20000000000LL ? (value / 1000LL) : value;
        return true;
    }
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        return playback_task_parse_epoch_from_string(item->valuestring, epoch_out);
    }

    return false;
}

static const cJSON *playback_task_find_array(const cJSON *root)
{
    const cJSON *data = NULL;

    if (cJSON_IsArray(root)) {
        return root;
    }

    data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsArray(data)) {
        return data;
    }
    if (cJSON_IsObject(data)) {
        const cJSON *records = cJSON_GetObjectItemCaseSensitive(data, "records");
        if (cJSON_IsArray(records)) {
            return records;
        }
    }

    data = cJSON_GetObjectItemCaseSensitive(root, "tasks");
    if (cJSON_IsArray(data)) {
        return data;
    }
    data = cJSON_GetObjectItemCaseSensitive(root, "records");
    if (cJSON_IsArray(data)) {
        return data;
    }

    return NULL;
}

static bool playback_task_parse_target_volume(const cJSON *item, uint8_t *volume_out)
{
    int value = 0;

    if ((item == NULL) || (volume_out == NULL) || !cJSON_IsNumber(item)) {
        return false;
    }

    value = item->valueint;
    if (value < 0) {
        value = 0;
    } else if (value > 100) {
        value = 100;
    }

    *volume_out = (uint8_t)value;
    return true;
}

static void playback_task_update_volume_from_response(const cJSON *root, const cJSON *task_array)
{
    uint8_t parsed_volume = 0;
    const cJSON *target_volume = NULL;
    int index = 0;

    if (root == NULL) {
        return;
    }

    target_volume = cJSON_GetObjectItemCaseSensitive(root, "targetVolume");
    if (playback_task_parse_target_volume(target_volume, &parsed_volume)) {
        s_current_volume_percent = parsed_volume;
        ESP_LOGI(TAG, "Alarm volume set to %u%% from targetVolume", (unsigned int)s_current_volume_percent);
        return;
    }

    if (!cJSON_IsArray(task_array)) {
        return;
    }

    for (index = 0; index < cJSON_GetArraySize(task_array); ++index) {
        const cJSON *task_object = cJSON_GetArrayItem(task_array, index);
        target_volume = cJSON_GetObjectItemCaseSensitive(task_object, "targetVolume");
        if (playback_task_parse_target_volume(target_volume, &parsed_volume)) {
            s_current_volume_percent = parsed_volume;
            ESP_LOGI(TAG, "Alarm volume set to %u%% from task targetVolume",
                     (unsigned int)s_current_volume_percent);
            return;
        }
    }
}

static bool playback_task_should_keep_remote_task(const playback_task_t *task,
                                                  time_t now,
                                                  uint32_t preload_window_hours)
{
    int64_t latest_epoch = (int64_t)now + ((int64_t)preload_window_hours * 3600LL);
    int64_t earliest_epoch = (int64_t)now - PLAYBACK_TASK_HTTP_GRACE_SECONDS;

    if (task->ring_at_epoch < earliest_epoch) {
        return false;
    }
    if (task->ring_at_epoch > latest_epoch) {
        return false;
    }
    if ((task->task_status == PLAYBACK_TASK_STATUS_FINISHED) ||
        (task->task_status == PLAYBACK_TASK_STATUS_FAILED)) {
        return false;
    }

    return true;
}

static void playback_task_merge_cached_fields(playback_task_t *new_task, const playback_task_t *existing_task)
{
    bool same_ring_at = false;
    bool same_audio = false;

    if ((new_task == NULL) || (existing_task == NULL)) {
        return;
    }

    same_ring_at = (new_task->ring_at_epoch == existing_task->ring_at_epoch);
    same_audio = (new_task->audio_hash != 0U) &&
                 (new_task->audio_hash == existing_task->audio_hash) &&
                 (strcmp(new_task->local_path, existing_task->local_path) == 0);

    if (!same_ring_at) {
        ESP_LOGI(TAG,
                 "Remote playback task reused instanceId=%s with changed ringAt; treating as new local schedule",
                 new_task->instance_id);
    }

    if (same_audio) {
        new_task->audio_status = existing_task->audio_status;
    }

    if (same_ring_at) {
        new_task->task_status = existing_task->task_status;
    }
}

static esp_err_t playback_task_parse_remote_object(const cJSON *task_object,
                                                   playback_task_t *task,
                                                   time_t now)
{
    const cJSON *instance_id = NULL;
    const cJSON *alarm_id = NULL;
    const cJSON *ring_at = NULL;
    const cJSON *title = NULL;
    const cJSON *audio_download_url = NULL;
    const cJSON *fallback_mode = NULL;
    const cJSON *status = NULL;
    int64_t ring_at_epoch = 0;

    if ((task_object == NULL) || (task == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    instance_id = cJSON_GetObjectItemCaseSensitive(task_object, "instanceId");
    ring_at = cJSON_GetObjectItemCaseSensitive(task_object, "ringAt");
    if (!cJSON_IsString(instance_id) || (instance_id->valuestring == NULL)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!playback_task_parse_ring_at(ring_at, &ring_at_epoch)) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(task, 0, sizeof(*task));
    task->ring_at_epoch = ring_at_epoch;
    task->expires_at_epoch = ring_at_epoch + PLAYBACK_TASK_KEEP_FILE_SECONDS;
    task->task_status = PLAYBACK_TASK_STATUS_PENDING;
    task->audio_status = PLAYBACK_AUDIO_STATUS_NONE;
    playback_task_copy_string(task->instance_id, sizeof(task->instance_id), instance_id->valuestring);

    alarm_id = cJSON_GetObjectItemCaseSensitive(task_object, "alarmId");
    title = cJSON_GetObjectItemCaseSensitive(task_object, "title");
    audio_download_url = cJSON_GetObjectItemCaseSensitive(task_object, "audioDownloadUrl");
    fallback_mode = cJSON_GetObjectItemCaseSensitive(task_object, "fallbackMode");
    status = cJSON_GetObjectItemCaseSensitive(task_object, "status");

    if (cJSON_IsString(alarm_id) && (alarm_id->valuestring != NULL)) {
        playback_task_copy_string(task->alarm_id, sizeof(task->alarm_id), alarm_id->valuestring);
    }
    if (cJSON_IsString(title) && (title->valuestring != NULL)) {
        playback_task_copy_string(task->title, sizeof(task->title), title->valuestring);
    }
    if (cJSON_IsString(audio_download_url) &&
        (audio_download_url->valuestring != NULL) &&
        (audio_download_url->valuestring[0] != '\0')) {
        task->audio_hash = audio_cache_service_hash_url(audio_download_url->valuestring);
        if (audio_cache_service_resolve_path(task->instance_id,
                                             audio_download_url->valuestring,
                                             task->local_path,
                                             sizeof(task->local_path)) != ESP_OK) {
            task->local_path[0] = '\0';
        }
        task->audio_status = PLAYBACK_AUDIO_STATUS_WAITING;
    } else {
        task->audio_status = PLAYBACK_AUDIO_STATUS_WAITING;
        ESP_LOGW(TAG,
                 "Remote playback task missing audioDownloadUrl instance=%s",
                 task->instance_id);
    }
    if (cJSON_IsString(fallback_mode) && (fallback_mode->valuestring != NULL)) {
        playback_task_copy_string(task->fallback_mode,
                                  sizeof(task->fallback_mode),
                                  fallback_mode->valuestring);
    }
    if (cJSON_IsString(status) && (status->valuestring != NULL)) {
        if (playback_task_string_equals_ignore_case(status->valuestring, "finished")) {
            task->task_status = PLAYBACK_TASK_STATUS_FINISHED;
        } else if (playback_task_string_equals_ignore_case(status->valuestring, "failed")) {
            task->task_status = PLAYBACK_TASK_STATUS_FAILED;
        } else if (playback_task_string_equals_ignore_case(status->valuestring, "playing")) {
            task->task_status = PLAYBACK_TASK_STATUS_PLAYING;
        }
    }

    if (task->ring_at_epoch < ((int64_t)now - PLAYBACK_TASK_HTTP_GRACE_SECONDS)) {
        task->expires_at_epoch = (int64_t)now;
    }

    return ESP_OK;
}

static const char *playback_task_status_to_string(playback_task_status_t status)
{
    switch (status) {
        case PLAYBACK_TASK_STATUS_READY:
            return "ready";
        case PLAYBACK_TASK_STATUS_PLAYING:
            return "playing";
        case PLAYBACK_TASK_STATUS_FINISHED:
            return "finished";
        case PLAYBACK_TASK_STATUS_FAILED:
            return "failed";
        case PLAYBACK_TASK_STATUS_PENDING:
        default:
            return "pending";
    }
}

static const char *playback_audio_status_to_string(playback_audio_status_t status)
{
    switch (status) {
        case PLAYBACK_AUDIO_STATUS_WAITING:
            return "waiting";
        case PLAYBACK_AUDIO_STATUS_CACHED:
            return "cached";
        case PLAYBACK_AUDIO_STATUS_FAILED:
            return "failed";
        case PLAYBACK_AUDIO_STATUS_NONE:
        default:
            return "none";
    }
}

static void playback_task_save_timer_callback(TimerHandle_t timer)
{
    (void)timer;

    if (s_task_handle != NULL) {
        xTaskNotify(s_task_handle, PLAYBACK_TASK_NOTIFY_SAVE, eSetBits);
    }
}

static void playback_task_flush_state_if_dirty(void)
{
    esp_err_t ret = ESP_OK;

    if (!s_state_dirty) {
        return;
    }

    ret = playback_task_save_state_to_nvs();
    if (ret == ESP_OK) {
        s_state_dirty = false;
        ESP_LOGI(TAG, "Persisted playback task state (%u tasks)", (unsigned int)s_task_count);
        return;
    }

    ESP_LOGW(TAG, "Failed to persist playback state: %s", esp_err_to_name(ret));
    if (s_save_timer != NULL) {
        xTimerStop(s_save_timer, 0);
        xTimerStart(s_save_timer, 0);
    }
}

static void playback_task_mark_dirty(bool immediate)
{
    s_state_dirty = true;

    if (immediate && (s_task_handle == xTaskGetCurrentTaskHandle())) {
        playback_task_flush_state_if_dirty();
        return;
    }

    if ((s_save_timer == NULL) || (s_task_handle == NULL)) {
        return;
    }

    if (immediate) {
        xTimerStop(s_save_timer, 0);
        xTaskNotify(s_task_handle, PLAYBACK_TASK_NOTIFY_SAVE, eSetBits);
        return;
    }

    if (xTimerIsTimerActive(s_save_timer) == pdTRUE) {
        xTimerReset(s_save_timer, 0);
    } else {
        xTimerStart(s_save_timer, 0);
    }
}

static bool playback_task_enqueue_report_status(const playback_task_t *task,
                                                playback_task_status_t reported_status)
{
    if (task == NULL) {
        return false;
    }

    network_task_service_request_playback_report(task->instance_id,
                                                 playback_task_status_to_string(reported_status),
                                                 playback_audio_status_to_string((playback_audio_status_t)task->audio_status));
    return true;
}

static esp_err_t playback_task_apply_cloud_response(const char *json,
                                                    playback_task_t *new_tasks,
                                                    size_t new_task_capacity,
                                                    network_task_audio_cache_item_t *cache_items,
                                                    size_t cache_item_capacity,
                                                    size_t *cache_item_count)
{
    device_cloud_config_t config = {0};
    cJSON *root = NULL;
    const cJSON *task_array = NULL;
    size_t new_task_count = 0;
    time_t now = time(NULL);
    size_t index = 0;
    bool changed = false;
    esp_err_t ret = ESP_OK;

    if ((json == NULL) || (new_tasks == NULL) || (new_task_capacity == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cache_item_count != NULL) {
        *cache_item_count = 0;
    }

    ret = device_cloud_service_get_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }
    ESP_LOGI(TAG, "pullPlaybackTasks raw JSON: %s", json);

    root = cJSON_Parse(json);
    if (root == NULL) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    task_array = playback_task_find_array(root);
    if (!cJSON_IsArray(task_array)) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    playback_task_update_volume_from_response(root, task_array);

    for (index = 0; index < (size_t)cJSON_GetArraySize(task_array) && new_task_count < new_task_capacity; ++index) {
        playback_task_t parsed_task = {0};
        const cJSON *task_object = cJSON_GetArrayItem(task_array, (int)index);
        const cJSON *audio_download_url = NULL;
        playback_task_t *existing_task = NULL;

        ret = playback_task_parse_remote_object(task_object, &parsed_task, now);
        if (ret != ESP_OK) {
            continue;
        }
        if (!playback_task_should_keep_remote_task(&parsed_task, now, config.preload_window_hours)) {
            continue;
        }

        existing_task = playback_task_find_by_instance(parsed_task.instance_id);
        playback_task_merge_cached_fields(&parsed_task, existing_task);
        if ((parsed_task.local_path[0] == '\0') &&
            (parsed_task.task_status == PLAYBACK_TASK_STATUS_READY)) {
            parsed_task.task_status = PLAYBACK_TASK_STATUS_PENDING;
        }
        audio_download_url = cJSON_GetObjectItemCaseSensitive(task_object, "audioDownloadUrl");
        if ((cache_items != NULL) &&
            (cache_item_count != NULL) &&
            (*cache_item_count < cache_item_capacity) &&
            cJSON_IsString(audio_download_url) &&
            (audio_download_url->valuestring != NULL) &&
            (audio_download_url->valuestring[0] != '\0')) {
            if (strlen(audio_download_url->valuestring) >= sizeof(cache_items[*cache_item_count].download_url)) {
                ESP_LOGW(TAG,
                         "Skipping audio download: audioDownloadUrl too long len=%u max=%u instance=%s",
                         (unsigned int)strlen(audio_download_url->valuestring),
                         (unsigned int)(sizeof(cache_items[*cache_item_count].download_url) - 1U),
                         parsed_task.instance_id);
                new_tasks[new_task_count++] = parsed_task;
                continue;
            }
            playback_task_copy_string(cache_items[*cache_item_count].download_url,
                                      sizeof(cache_items[*cache_item_count].download_url),
                                      audio_download_url->valuestring);
            cache_items[*cache_item_count].ring_at_epoch = parsed_task.ring_at_epoch;
            ESP_LOGI(TAG,
                     "Queued audioDownloadUrl for instance=%s url=%s",
                     parsed_task.instance_id,
                     audio_download_url->valuestring);
            ++(*cache_item_count);
        }
        new_tasks[new_task_count++] = parsed_task;
    }

    if (new_task_count > 1U) {
        qsort(new_tasks, new_task_count, sizeof(playback_task_t), playback_task_compare_ring_time);
    }

    changed = (new_task_count != s_task_count);
    if (!changed && (new_task_count > 0U)) {
        changed = memcmp(s_tasks, new_tasks, new_task_count * sizeof(playback_task_t)) != 0;
    }

    if (changed) {
        memset(s_tasks, 0, sizeof(s_tasks));
        s_task_count = new_task_count;
        if (new_task_count > 0U) {
            memcpy(s_tasks, new_tasks, new_task_count * sizeof(playback_task_t));
        }
        if (playback_task_remove_expired(now)) {
            changed = true;
        }
        playback_task_mark_dirty(false);
    } else if (playback_task_remove_expired(now)) {
        playback_task_mark_dirty(false);
    }

    ESP_LOGI(TAG, "Synced %u playback tasks", (unsigned int)s_task_count);
    playback_task_log_next_due(now);
    ret = ESP_OK;

cleanup:
    cJSON_Delete(root);
    return ret;
}

static bool playback_task_find_cached_audio(playback_task_t *task,
                                            char *path_buffer,
                                            size_t path_buffer_size)
{
    if ((task == NULL) || (task->local_path[0] == '\0')) {
        return false;
    }
    if (!audio_cache_service_file_exists(task->local_path)) {
        return false;
    }

    if ((path_buffer != NULL) && (path_buffer_size > 0)) {
        playback_task_copy_string(path_buffer, path_buffer_size, task->local_path);
    }
    return true;
}

static void playback_task_request_audio_cache_maintenance(const network_task_audio_cache_item_t *items,
                                                          size_t item_count)
{
    if ((items == NULL) || (item_count == 0U) || !audio_cache_service_is_ready()) {
        return;
    }

    network_task_service_request_audio_cache_maintenance(items,
                                                         item_count,
                                                         s_current_playing_path);
}

static esp_err_t playback_task_play_now(playback_task_t *task)
{
    const char *play_path = NULL;
    char cached_path[AUDIO_CACHE_PATH_MAX] = {0};
    audio_service_format_t format = AUDIO_SERVICE_FORMAT_AUTO;
    esp_err_t ret = ESP_OK;

    if (task == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (playback_task_find_cached_audio(task, cached_path, sizeof(cached_path))) {
        play_path = cached_path;
        ESP_LOGI(TAG,
                 "Playback source: cached audio path=%s volume=%u",
                 play_path,
                 (unsigned int)s_current_volume_percent);
    } else if (playback_task_allows_fallback(task) && storage_service_default_audio_exists()) {
        play_path = storage_service_get_default_audio_path();
        format = AUDIO_SERVICE_FORMAT_AUTO;
        ESP_LOGI(TAG,
                 "Playback source: default audio path=%s fallbackMode=%s volume=%u",
                 play_path,
                 task->fallback_mode,
                 (unsigned int)s_current_volume_percent);
    } else {
        ESP_LOGW(TAG,
                 "No playable audio for %s: localPath=%s audioHash=%" PRIu32 " fallbackMode=%s fallbackAllowed=%d defaultPath=%s defaultExists=%d",
                 task->instance_id,
                 task->local_path,
                 task->audio_hash,
                 task->fallback_mode,
                 playback_task_allows_fallback(task) ? 1 : 0,
                 storage_service_get_default_audio_path(),
                 storage_service_default_audio_exists() ? 1 : 0);
        task->task_status = PLAYBACK_TASK_STATUS_FAILED;
        task->audio_status = PLAYBACK_AUDIO_STATUS_FAILED;
        task->expires_at_epoch = time(NULL) + PLAYBACK_TASK_KEEP_FILE_SECONDS;
        playback_task_mark_dirty(true);
        ESP_LOGW(TAG, "Playback failed locally for %s: no playable audio (Failed: with no audio)", task->instance_id);
        return ESP_ERR_NOT_FOUND;
    }

    task->task_status = PLAYBACK_TASK_STATUS_PLAYING;

    playback_task_copy_string(s_current_playing_path, sizeof(s_current_playing_path), play_path);
    ret = audio_service_play(play_path, format, s_current_volume_percent, 0);
    s_current_playing_path[0] = '\0';
    if ((ret != ESP_OK) &&
        (format == AUDIO_SERVICE_FORMAT_AUTO) &&
        playback_task_allows_fallback(task) &&
        storage_service_default_audio_exists() &&
        (strcmp(play_path, storage_service_get_default_audio_path()) != 0)) {
        task->audio_status = PLAYBACK_AUDIO_STATUS_FAILED;
        playback_task_copy_string(s_current_playing_path,
                                  sizeof(s_current_playing_path),
                                  storage_service_get_default_audio_path());
        ret = audio_service_play(storage_service_get_default_audio_path(),
                                 AUDIO_SERVICE_FORMAT_AUTO,
                                 s_current_volume_percent,
                                 0);
        s_current_playing_path[0] = '\0';
    }

    if (ret == ESP_OK) {
        task->task_status = PLAYBACK_TASK_STATUS_FINISHED;
    } else {
        task->task_status = PLAYBACK_TASK_STATUS_FAILED;
        task->audio_status = PLAYBACK_AUDIO_STATUS_FAILED;
    }

    task->expires_at_epoch = time(NULL) + PLAYBACK_TASK_KEEP_FILE_SECONDS;
    playback_task_mark_dirty(true);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Playback finished locally for %s (Success)", task->instance_id);
    } else {
        ESP_LOGW(TAG,
                 "Playback failed locally for %s: %s (Failed: play failed)",
                 task->instance_id,
                 esp_err_to_name(ret));
    }

    return ret;
}

static bool playback_task_process_due_tasks(time_t now)
{
    size_t index = 0;
    bool played_task = false;

    if (!time_service_has_valid_time()) {
        return false;
    }

    for (index = 0; index < s_task_count; ++index) {
        playback_task_t *task = &s_tasks[index];

        if ((task->task_status == PLAYBACK_TASK_STATUS_FINISHED) ||
            (task->task_status == PLAYBACK_TASK_STATUS_FAILED) ||
            (task->task_status == PLAYBACK_TASK_STATUS_PLAYING)) {
            continue;
        }
        if (task->ring_at_epoch > (int64_t)now) {
            break;
        }

        playback_task_play_now(task);
        played_task = true;
    }

    return played_task;
}

static void playback_task_apply_pull_result(esp_err_t result, const char *json)
{
    playback_task_t *scratch_tasks = NULL;
    network_task_audio_cache_item_t *cache_items = NULL;
    size_t cache_item_count = 0;

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "pullPlaybackTasks failed: %s", esp_err_to_name(result));
        return;
    }

    scratch_tasks = calloc(PLAYBACK_TASK_MAX_COUNT, sizeof(*scratch_tasks));
    if (scratch_tasks == NULL) {
        ESP_LOGE(TAG,
                 "Failed to allocate playback sync scratch tasks bytes=%u free=%u largest=%u",
                 (unsigned int)(PLAYBACK_TASK_MAX_COUNT * sizeof(*scratch_tasks)),
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }

    cache_items = calloc(PLAYBACK_TASK_MAX_COUNT, sizeof(*cache_items));
    if (cache_items == NULL) {
        ESP_LOGW(TAG,
                 "Failed to allocate audio cache queue scratch; tasks will sync without new downloads");
    }

    if (playback_task_apply_cloud_response(json,
                                           scratch_tasks,
                                           PLAYBACK_TASK_MAX_COUNT,
                                           cache_items,
                                           cache_items == NULL ? 0U : PLAYBACK_TASK_MAX_COUNT,
                                           &cache_item_count) == ESP_OK) {
        playback_task_request_audio_cache_maintenance(cache_items, cache_item_count);
    }

    free(cache_items);
    free(scratch_tasks);
}

static void playback_task_network_pull_result(esp_err_t ret, const char *json, size_t json_len, void *ctx)
{
    (void)ctx;
    (void)json_len;

    taskENTER_CRITICAL(&s_pull_result_lock);
    s_startup_pull_result_seen = true;
    taskEXIT_CRITICAL(&s_pull_result_lock);

    if ((s_task_handle == NULL) || (s_pull_result_done_sem == NULL)) {
        playback_task_apply_pull_result(ret, json);
        return;
    }

    taskENTER_CRITICAL(&s_pull_result_lock);
    s_pull_result_ret = ret;
    s_pull_result_json = json;
    s_pull_result_pending = true;
    taskEXIT_CRITICAL(&s_pull_result_lock);

    if (s_task_handle != NULL) {
        xTaskNotify(s_task_handle, PLAYBACK_TASK_NOTIFY_PULL_RESULT, eSetBits);
    }
    if (xSemaphoreTake(s_pull_result_done_sem, pdMS_TO_TICKS(15000)) != pdTRUE) {
        taskENTER_CRITICAL(&s_pull_result_lock);
        s_pull_result_pending = false;
        s_pull_result_json = NULL;
        taskEXIT_CRITICAL(&s_pull_result_lock);
        ESP_LOGW(TAG, "Timed out waiting for playback task to consume pull result");
    }
}

bool playback_task_service_has_pending_pull_result(void)
{
    bool pending = false;

    taskENTER_CRITICAL(&s_pull_result_lock);
    pending = s_pull_result_pending;
    taskEXIT_CRITICAL(&s_pull_result_lock);

    return pending;
}

static bool playback_task_has_startup_pull_result_seen(void)
{
    bool seen = false;

    taskENTER_CRITICAL(&s_pull_result_lock);
    seen = s_startup_pull_result_seen;
    taskEXIT_CRITICAL(&s_pull_result_lock);

    return seen;
}

static void playback_task_handle_pending_pull_result(void)
{
    esp_err_t result = ESP_OK;
    const char *json = NULL;
    bool pending = false;

    taskENTER_CRITICAL(&s_pull_result_lock);
    pending = s_pull_result_pending;
    if (pending) {
        result = s_pull_result_ret;
        json = s_pull_result_json;
        s_pull_result_pending = false;
        s_pull_result_json = NULL;
    }
    taskEXIT_CRITICAL(&s_pull_result_lock);

    if (!pending) {
        return;
    }

    playback_task_apply_pull_result(result, json);
    if (s_pull_result_done_sem != NULL) {
        xSemaphoreGive(s_pull_result_done_sem);
    }
}

static void playback_task_apply_audio_result(esp_err_t ret, const char *local_path)
{
    size_t index = 0;
    bool changed = false;
    bool should_report_ready = false;
    const char *status_text = NULL;

    if ((local_path == NULL) || (local_path[0] == '\0')) {
        return;
    }

    if ((ret == ESP_OK) && (local_path != NULL) && (local_path[0] != '\0')) {
        status_text = "cached";
    } else if ((ret == ESP_ERR_NO_MEM) || (ret == ESP_ERR_NOT_FOUND) || (ret == ESP_ERR_INVALID_STATE)) {
        status_text = "waiting";
    } else {
        status_text = "failed";
    }
    ESP_LOGI(TAG,
             "Audio cache result: status=%s ret=%s path=%s",
             status_text,
             esp_err_to_name(ret),
             local_path);

    for (index = 0; index < s_task_count; ++index) {
        playback_task_t *task = &s_tasks[index];

        if (strcmp(task->local_path, local_path) != 0) {
            continue;
        }

        if ((ret == ESP_OK) && (local_path != NULL) && (local_path[0] != '\0')) {
            if (task->audio_status != PLAYBACK_AUDIO_STATUS_CACHED) {
                task->audio_status = PLAYBACK_AUDIO_STATUS_CACHED;
                changed = true;
            }
            if (task->task_status == PLAYBACK_TASK_STATUS_PENDING) {
                task->task_status = PLAYBACK_TASK_STATUS_READY;
                changed = true;
                should_report_ready = true;
            }
        } else {
            uint8_t next_audio_status = ((ret == ESP_ERR_NO_MEM) ||
                                         (ret == ESP_ERR_NOT_FOUND) ||
                                         (ret == ESP_ERR_INVALID_STATE)) ?
                                        PLAYBACK_AUDIO_STATUS_WAITING :
                                        PLAYBACK_AUDIO_STATUS_FAILED;
            if (task->audio_status != next_audio_status) {
                task->audio_status = next_audio_status;
                changed = true;
            }
            if (task->task_status == PLAYBACK_TASK_STATUS_READY) {
                task->task_status = PLAYBACK_TASK_STATUS_PENDING;
                changed = true;
            }
        }
    }

    if (changed) {
        playback_task_mark_dirty(false);
    }
    if (should_report_ready) {
        for (index = 0; index < s_task_count; ++index) {
            playback_task_t *task = &s_tasks[index];
            if ((strcmp(task->local_path, local_path) == 0) &&
                (task->task_status == PLAYBACK_TASK_STATUS_READY)) {
                playback_task_enqueue_report_status(task, PLAYBACK_TASK_STATUS_READY);
            }
        }
    }
}

static bool playback_task_take_audio_result(playback_audio_result_t *result)
{
    bool found = false;

    if (result == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_audio_result_lock);
    if (s_audio_result.in_use) {
        *result = s_audio_result;
        memset(&s_audio_result, 0, sizeof(s_audio_result));
        found = true;
    }
    taskEXIT_CRITICAL(&s_audio_result_lock);

    return found;
}

static void playback_task_handle_pending_audio_results(void)
{
    playback_audio_result_t result = {0};

    while (playback_task_take_audio_result(&result)) {
        playback_task_apply_audio_result(result.ret, result.local_path);
        if (s_audio_result_done_sem != NULL) {
            xSemaphoreGive(s_audio_result_done_sem);
        }
    }
}

static void playback_task_network_audio_result(const char *download_url,
                                               esp_err_t ret,
                                               const char *local_path,
                                               void *ctx)
{
    (void)ctx;
    (void)download_url;

    if ((local_path == NULL) || (local_path[0] == '\0')) {
        return;
    }

    if ((s_task_handle == NULL) || (s_audio_result_done_sem == NULL)) {
        playback_task_apply_audio_result(ret, local_path);
        return;
    }

    taskENTER_CRITICAL(&s_audio_result_lock);
    s_audio_result.in_use = true;
    s_audio_result.ret = ret;
    playback_task_copy_string(s_audio_result.local_path,
                              sizeof(s_audio_result.local_path),
                              local_path);
    taskEXIT_CRITICAL(&s_audio_result_lock);

    if (s_task_handle != NULL) {
        xTaskNotify(s_task_handle, PLAYBACK_TASK_NOTIFY_AUDIO_RESULT, eSetBits);
    }
    if (xSemaphoreTake(s_audio_result_done_sem, pdMS_TO_TICKS(15000)) != pdTRUE) {
        taskENTER_CRITICAL(&s_audio_result_lock);
        memset(&s_audio_result, 0, sizeof(s_audio_result));
        taskEXIT_CRITICAL(&s_audio_result_lock);
        ESP_LOGW(TAG, "Timed out waiting for playback task to consume audio result path=%s", local_path);
    }
}

static int64_t playback_task_next_due_epoch(time_t now)
{
    size_t index = 0;

    for (index = 0; index < s_task_count; ++index) {
        const playback_task_t *task = &s_tasks[index];

        if ((task->task_status == PLAYBACK_TASK_STATUS_FINISHED) ||
            (task->task_status == PLAYBACK_TASK_STATUS_FAILED) ||
            (task->task_status == PLAYBACK_TASK_STATUS_PLAYING)) {
            continue;
        }
        if (task->ring_at_epoch > (int64_t)now) {
            return task->ring_at_epoch;
        }
        if (task->ring_at_epoch <= (int64_t)now) {
            return (int64_t)now;
        }
    }

    return 0;
}

static void playback_task_log_next_due(time_t now)
{
    int64_t next_due_epoch = 0;
    size_t index = 0;

    next_due_epoch = playback_task_next_due_epoch(now);
    if (next_due_epoch <= 0) {
        ESP_LOGI(TAG, "No pending playback task in local queue");
        return;
    }

    for (index = 0; index < s_task_count; ++index) {
        const playback_task_t *task = &s_tasks[index];

        if ((task->task_status == PLAYBACK_TASK_STATUS_FINISHED) ||
            (task->task_status == PLAYBACK_TASK_STATUS_FAILED) ||
            (task->task_status == PLAYBACK_TASK_STATUS_PLAYING)) {
            continue;
        }
        if (task->ring_at_epoch == next_due_epoch) {
            struct tm local_time = {0};
            char time_buffer[32] = {0};
            time_t printable_time = (time_t)task->ring_at_epoch;

            if (localtime_r(&printable_time, &local_time) != NULL) {
                strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &local_time);
            } else {
                snprintf(time_buffer, sizeof(time_buffer), "%" PRId64, task->ring_at_epoch);
            }

            ESP_LOGI(TAG,
                     "Next due task: instance=%s ringAt=%s status=%s audio=%s",
                     task->instance_id,
                     time_buffer,
                     playback_task_status_to_string((playback_task_status_t)task->task_status),
                     playback_audio_status_to_string((playback_audio_status_t)task->audio_status));
            return;
        }
    }

    ESP_LOGI(TAG, "Next due epoch=%" PRId64 " but no matching task found", next_due_epoch);
}

static TickType_t playback_task_compute_wait_ticks(time_t now, time_t next_sync_epoch)
{
    int64_t next_due_epoch = 0;
    int64_t wake_epoch = 0;
    uint32_t wait_seconds = 0;

    if (!playback_task_is_connected()) {
        return portMAX_DELAY;
    }
    if (!time_service_has_valid_time()) {
        return pdMS_TO_TICKS(PLAYBACK_TASK_TIME_WAIT_MS);
    }
    if (s_force_sync || (next_sync_epoch == 0) || (now >= next_sync_epoch)) {
        return 0;
    }

    wake_epoch = next_sync_epoch;
    next_due_epoch = playback_task_next_due_epoch(now);
    if ((next_due_epoch > 0) && (next_due_epoch < wake_epoch)) {
        wake_epoch = next_due_epoch;
    }
    if (wake_epoch <= (int64_t)now) {
        return 0;
    }

    wait_seconds = (uint32_t)(wake_epoch - (int64_t)now);
    return pdMS_TO_TICKS(wait_seconds * 1000U);
}

static void playback_task_service_task(void *arg)
{
    time_t next_sync_epoch = 0;
    bool initial_sync_attempted = false;

    (void)arg;
    ESP_LOGI(TAG,
             "playback_task stack_free=%u bytes",
             (unsigned int)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    while (true) {
        device_cloud_config_t config = {0};
        uint32_t notify_bits = 0;
        esp_err_t config_ret = ESP_OK;
        time_t now = 0;

        xEventGroupWaitBits(s_connected_event_group,
                            s_connected_bit,
                            pdFALSE,
                            pdTRUE,
                            portMAX_DELAY);

        playback_task_handle_pending_pull_result();
        playback_task_handle_pending_audio_results();

        if (device_cloud_service_get_generation() != s_last_config_generation) {
            s_last_config_generation = device_cloud_service_get_generation();
            s_force_sync = true;
            initial_sync_attempted = false;
        }

        if (!time_service_has_valid_time()) {
            if (xTaskNotifyWait(0, UINT32_MAX, &notify_bits, pdMS_TO_TICKS(PLAYBACK_TASK_TIME_WAIT_MS)) == pdTRUE) {
                if ((notify_bits & PLAYBACK_TASK_NOTIFY_SYNC) != 0U) {
                    s_force_sync = true;
                }
                if ((notify_bits & PLAYBACK_TASK_NOTIFY_SAVE) != 0U) {
                    playback_task_flush_state_if_dirty();
                }
                if ((notify_bits & PLAYBACK_TASK_NOTIFY_PULL_RESULT) != 0U) {
                    playback_task_handle_pending_pull_result();
                }
                if ((notify_bits & PLAYBACK_TASK_NOTIFY_AUDIO_RESULT) != 0U) {
                    playback_task_handle_pending_audio_results();
                }
            }
            continue;
        }

        now = time(NULL);
        if (playback_task_remove_expired(now)) {
            playback_task_mark_dirty(false);
        }

        config_ret = device_cloud_service_get_config(&config);
        if (!initial_sync_attempted && playback_task_has_startup_pull_result_seen()) {
            initial_sync_attempted = true;
            next_sync_epoch = now + (time_t)((config_ret == ESP_OK && config.task_poll_seconds > 0U) ?
                                             config.task_poll_seconds :
                                             (5U * 60U));
            s_force_sync = false;
        }
        if ((config_ret == ESP_OK) &&
            playback_task_is_connected() &&
            (s_force_sync || (next_sync_epoch == 0) || (now >= next_sync_epoch))) {
            network_task_service_request_playback_pull(NETWORK_TASK_PLAYBACK_REASON_NORMAL);
            initial_sync_attempted = true;
            now = time(NULL);
            next_sync_epoch = now + (time_t)(config.task_poll_seconds > 0U ? config.task_poll_seconds : (5U * 60U));
            s_force_sync = false;
        } else if (!initial_sync_attempted && (config_ret != ESP_OK)) {
            ESP_LOGW(TAG,
                     "Skipping initial cloud sync because config is unavailable: %s",
                     esp_err_to_name(config_ret));
            initial_sync_attempted = true;
        }

        if (initial_sync_attempted) {
            int64_t due_epoch = playback_task_next_due_epoch(now);

            if ((due_epoch > 0) && (due_epoch <= (int64_t)now)) {
                ESP_LOGI(TAG, "Releasing cloud HTTP session before local audio playback");
                network_task_service_reset_sessions();

                if (playback_task_process_due_tasks(now)) {
                    now = time(NULL);
                    s_force_sync = false;
                    network_task_service_request_playback_pull(NETWORK_TASK_PLAYBACK_REASON_POST_PLAYBACK);
                    next_sync_epoch = now + (time_t)(config.task_poll_seconds > 0U ? config.task_poll_seconds : (5U * 60U));
                    ESP_LOGI(TAG, "Playback completed locally; queued post playback cloud sync");
                }
            }
        }

        playback_task_flush_state_if_dirty();

        if (xTaskNotifyWait(0,
                            UINT32_MAX,
                            &notify_bits,
                            playback_task_compute_wait_ticks(time(NULL), next_sync_epoch)) == pdTRUE) {
            if ((notify_bits & PLAYBACK_TASK_NOTIFY_SYNC) != 0U) {
                s_force_sync = true;
            }
            if ((notify_bits & PLAYBACK_TASK_NOTIFY_SAVE) != 0U) {
                playback_task_flush_state_if_dirty();
            }
            if ((notify_bits & PLAYBACK_TASK_NOTIFY_PULL_RESULT) != 0U) {
                playback_task_handle_pending_pull_result();
            }
            if ((notify_bits & PLAYBACK_TASK_NOTIFY_AUDIO_RESULT) != 0U) {
                playback_task_handle_pending_audio_results();
            }
        }
    }
}

esp_err_t playback_task_service_init(void)
{
    if (s_pull_result_done_sem == NULL) {
        s_pull_result_done_sem = xSemaphoreCreateBinary();
        if (s_pull_result_done_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_audio_result_done_sem == NULL) {
        s_audio_result_done_sem = xSemaphoreCreateBinary();
        if (s_audio_result_done_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    playback_task_load_state();
    s_state_dirty = false;
    network_task_service_register_playback_pull_handler(playback_task_network_pull_result, NULL);
    network_task_service_register_audio_cache_handler(playback_task_network_audio_result, NULL);
    return ESP_OK;
}

esp_err_t playback_task_service_start(EventGroupHandle_t connected_event_group, EventBits_t connected_bit)
{
    if ((connected_event_group == NULL) || (connected_bit == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_task_handle != NULL) {
        return ESP_OK;
    }

    s_connected_event_group = connected_event_group;
    s_connected_bit = connected_bit;
    s_last_config_generation = device_cloud_service_get_generation();

    s_save_timer = xTimerCreate("playback_save",
                                pdMS_TO_TICKS(PLAYBACK_TASK_SAVE_DELAY_MS),
                                pdFALSE,
                                NULL,
                                playback_task_save_timer_callback);
    if (s_save_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(playback_task_service_task,
                    "playback_task",
                    PLAYBACK_TASK_TASK_STACK_SIZE,
                    NULL,
                    PLAYBACK_TASK_TASK_PRIORITY,
                    &s_task_handle) != pdPASS) {
        xTimerDelete(s_save_timer, 0);
        s_save_timer = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (playback_task_service_has_pending_pull_result()) {
        xTaskNotify(s_task_handle, PLAYBACK_TASK_NOTIFY_PULL_RESULT, eSetBits);
    }

    return ESP_OK;
}

void playback_task_service_request_sync(void)
{
    s_force_sync = true;
    if (s_task_handle != NULL) {
        xTaskNotify(s_task_handle, PLAYBACK_TASK_NOTIFY_SYNC, eSetBits);
    }
}
