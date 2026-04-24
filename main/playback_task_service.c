#include "playback_task_service.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_cache_service.h"
#include "audio_service.h"
#include "cJSON.h"
#include "device_cloud_service.h"
#include "esp_log.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "storage_service.h"
#include "time_service.h"

static const char *TAG = "PLAYBACK_TASK";
static const char *PLAYBACK_TASK_NAMESPACE = "playback";
static const char *PLAYBACK_TASK_BLOB_KEY = "tasks_v1";

#define PLAYBACK_TASK_TASK_STACK_SIZE             8192
#define PLAYBACK_TASK_TASK_PRIORITY               4
#define PLAYBACK_TASK_REPORT_TASK_STACK_SIZE      6144
#define PLAYBACK_TASK_REPORT_TASK_PRIORITY        4
#define PLAYBACK_TASK_RESPONSE_BYTES              4096
#define PLAYBACK_TASK_REPORT_RESPONSE_BYTES       1024
#define PLAYBACK_TASK_HTTP_GRACE_SECONDS          300
#define PLAYBACK_TASK_KEEP_FILE_SECONDS           (24 * 60 * 60)
#define PLAYBACK_TASK_NOTIFY_SYNC                 BIT0
#define PLAYBACK_TASK_NOTIFY_SAVE                 BIT1
#define PLAYBACK_TASK_TIME_WAIT_MS                5000
#define PLAYBACK_TASK_SAVE_DELAY_MS               3000
#define PLAYBACK_TASK_REPORT_QUEUE_LENGTH         (PLAYBACK_TASK_MAX_COUNT * 2)
#define PLAYBACK_TASK_REPORT_INITIAL_RETRY_SECS   30
#define PLAYBACK_TASK_REPORT_MAX_RETRY_SECS       (15 * 60)

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t task_count;
    playback_task_t tasks[PLAYBACK_TASK_MAX_COUNT];
} playback_task_blob_t;

typedef struct {
    char instance_id[PLAYBACK_TASK_ID_SIZE];
    uint8_t task_status;
    uint8_t audio_status;
    uint8_t retry_count;
} playback_report_event_t;

static playback_task_t s_tasks[PLAYBACK_TASK_MAX_COUNT];
static size_t s_task_count;
static EventGroupHandle_t s_connected_event_group;
static EventBits_t s_connected_bit;
static TaskHandle_t s_task_handle;
static TaskHandle_t s_report_task_handle;
static QueueHandle_t s_report_queue;
static TimerHandle_t s_save_timer;
static bool s_force_sync = true;
static bool s_state_dirty;
static uint32_t s_last_config_generation;

static void playback_task_mark_dirty(bool immediate);
static void playback_task_flush_state_if_dirty(void);

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
    esp_err_t ret = ESP_OK;

    blob = calloc(1, sizeof(*blob));
    if (blob == NULL) {
        return ESP_ERR_NO_MEM;
    }

    blob->magic = 0x50544231U;
    blob->version = 1;
    blob->task_count = (uint32_t)s_task_count;
    if (s_task_count > 0) {
        memcpy(blob->tasks, s_tasks, sizeof(playback_task_t) * s_task_count);
    }

    ret = nvs_open(PLAYBACK_TASK_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        free(blob);
        return ret;
    }

    ret = nvs_set_blob(handle, PLAYBACK_TASK_BLOB_KEY, blob, sizeof(*blob));
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
    bool changed = false;

    for (read_index = 0; read_index < s_task_count; ++read_index) {
        const playback_task_t *task = &s_tasks[read_index];
        if ((task->expires_at_epoch > 0) && (task->expires_at_epoch < now)) {
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
            (blob->version == 1U) &&
            (blob->task_count <= PLAYBACK_TASK_MAX_COUNT)) {
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
    if ((new_task == NULL) || (existing_task == NULL)) {
        return;
    }

    playback_task_copy_string(new_task->local_audio_path,
                              sizeof(new_task->local_audio_path),
                              existing_task->local_audio_path);
    new_task->audio_cached = existing_task->audio_cached;
    new_task->audio_status = existing_task->audio_status;
    new_task->task_status = existing_task->task_status;
    new_task->last_reported_status = existing_task->last_reported_status;
    new_task->retry_count = existing_task->retry_count;

    if ((new_task->audio_cached != 0U) &&
        !audio_cache_service_file_exists(new_task->local_audio_path)) {
        new_task->audio_cached = 0U;
        new_task->audio_status = PLAYBACK_AUDIO_STATUS_WAITING;
        new_task->local_audio_path[0] = '\0';
        if (new_task->task_status == PLAYBACK_TASK_STATUS_READY) {
            new_task->task_status = PLAYBACK_TASK_STATUS_PENDING;
        }
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
    const cJSON *audio_url = NULL;
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
    task->last_reported_status = PLAYBACK_TASK_STATUS_REPORTED;
    task->ring_at_epoch = ring_at_epoch;
    task->expires_at_epoch = ring_at_epoch + PLAYBACK_TASK_KEEP_FILE_SECONDS;
    task->task_status = PLAYBACK_TASK_STATUS_PENDING;
    task->audio_status = PLAYBACK_AUDIO_STATUS_NONE;
    playback_task_copy_string(task->instance_id, sizeof(task->instance_id), instance_id->valuestring);

    alarm_id = cJSON_GetObjectItemCaseSensitive(task_object, "alarmId");
    title = cJSON_GetObjectItemCaseSensitive(task_object, "title");
    audio_url = cJSON_GetObjectItemCaseSensitive(task_object, "audioUrl");
    fallback_mode = cJSON_GetObjectItemCaseSensitive(task_object, "fallbackMode");
    status = cJSON_GetObjectItemCaseSensitive(task_object, "status");

    if (cJSON_IsString(alarm_id) && (alarm_id->valuestring != NULL)) {
        playback_task_copy_string(task->alarm_id, sizeof(task->alarm_id), alarm_id->valuestring);
    }
    if (cJSON_IsString(title) && (title->valuestring != NULL)) {
        playback_task_copy_string(task->title, sizeof(task->title), title->valuestring);
    }
    if (cJSON_IsString(audio_url) && (audio_url->valuestring != NULL) && (audio_url->valuestring[0] != '\0')) {
        playback_task_copy_string(task->audio_url, sizeof(task->audio_url), audio_url->valuestring);
        task->audio_status = PLAYBACK_AUDIO_STATUS_WAITING;
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

static uint32_t playback_task_report_retry_seconds(uint8_t retry_count)
{
    uint32_t seconds = PLAYBACK_TASK_REPORT_INITIAL_RETRY_SECS;

    while ((retry_count > 0U) && (seconds < PLAYBACK_TASK_REPORT_MAX_RETRY_SECS)) {
        seconds *= 2U;
        --retry_count;
    }
    if (seconds > PLAYBACK_TASK_REPORT_MAX_RETRY_SECS) {
        seconds = PLAYBACK_TASK_REPORT_MAX_RETRY_SECS;
    }

    return seconds;
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
    playback_report_event_t event = {0};

    if ((task == NULL) || (s_report_queue == NULL)) {
        return false;
    }

    playback_task_copy_string(event.instance_id, sizeof(event.instance_id), task->instance_id);
    event.task_status = (uint8_t)reported_status;
    event.audio_status = task->audio_status;
    event.retry_count = 0;

    if (xQueueSendToBack(s_report_queue, &event, 0) != pdPASS) {
        ESP_LOGW(TAG, "Report queue full, dropping status=%s for %s",
                 playback_task_status_to_string(reported_status),
                 task->instance_id);
        return false;
    }

    return true;
}

static esp_err_t playback_task_build_report_json(const device_cloud_config_t *config,
                                                 const playback_report_event_t *event,
                                                 char *buffer,
                                                 size_t buffer_size)
{
    int written = 0;

    if ((config == NULL) || (event == NULL) || (buffer == NULL) || (buffer_size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (event->audio_status != PLAYBACK_AUDIO_STATUS_NONE) {
        written = snprintf(buffer,
                           buffer_size,
                           "{\"clientId\":\"%s\",\"deviceId\":\"%s\",\"instanceId\":\"%s\","
                           "\"status\":\"%s\",\"audioStatus\":\"%s\"}",
                           config->client_id,
                           config->device_id,
                           event->instance_id,
                           playback_task_status_to_string((playback_task_status_t)event->task_status),
                           playback_audio_status_to_string((playback_audio_status_t)event->audio_status));
    } else {
        written = snprintf(buffer,
                           buffer_size,
                           "{\"clientId\":\"%s\",\"deviceId\":\"%s\",\"instanceId\":\"%s\","
                           "\"status\":\"%s\"}",
                           config->client_id,
                           config->device_id,
                           event->instance_id,
                           playback_task_status_to_string((playback_task_status_t)event->task_status));
    }

    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static void playback_task_report_task(void *arg)
{
    playback_report_event_t event = {0};
    char request_body[512] = {0};
    char *response_buffer = NULL;
    device_cloud_session_t session = {0};
    device_cloud_http_response_t response = {0};

    (void)arg;

    response_buffer = calloc(1, PLAYBACK_TASK_REPORT_RESPONSE_BYTES);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate playback report buffer");
        vTaskDelete(NULL);
        return;
    }

    response.buffer = response_buffer;
    response.buffer_size = PLAYBACK_TASK_REPORT_RESPONSE_BYTES;
    device_cloud_session_init(&session, response_buffer, PLAYBACK_TASK_REPORT_RESPONSE_BYTES);

    while (true) {
        device_cloud_config_t config = {0};
        esp_err_t ret = ESP_OK;

        if (xQueueReceive(s_report_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!playback_task_is_connected()) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            ret = device_cloud_service_get_config(&config);
            if ((ret == ESP_OK) && (config.report_status_url[0] == '\0')) {
                ret = ESP_ERR_INVALID_STATE;
            }
            if (ret == ESP_OK) {
                ret = playback_task_build_report_json(&config, &event, request_body, sizeof(request_body));
            }
            if (ret == ESP_OK) {
                ret = device_cloud_session_post_json(&session,
                                                     config.report_status_url,
                                                     request_body,
                                                     &response);
            }
        }

        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Reported %s for %s",
                     playback_task_status_to_string((playback_task_status_t)event.task_status),
                     event.instance_id);
            continue;
        }

        event.retry_count++;
        {
            uint32_t retry_seconds = playback_task_report_retry_seconds(event.retry_count);
            ESP_LOGW(TAG,
                     "Report failed for %s status=%s retry=%u in %u s: %s",
                     event.instance_id,
                     playback_task_status_to_string((playback_task_status_t)event.task_status),
                     (unsigned int)event.retry_count,
                     (unsigned int)retry_seconds,
                     esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(retry_seconds * 1000U));
        }

        if (xQueueSendToFront(s_report_queue, &event, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to requeue report for %s", event.instance_id);
        }
    }
}

static esp_err_t playback_task_sync_from_cloud(device_cloud_session_t *session,
                                               device_cloud_http_response_t *response,
                                               playback_task_t *new_tasks,
                                               size_t new_task_capacity)
{
    device_cloud_config_t config = {0};
    char request_body[256] = {0};
    cJSON *root = NULL;
    const cJSON *task_array = NULL;
    size_t new_task_count = 0;
    time_t now = time(NULL);
    size_t index = 0;
    bool changed = false;
    esp_err_t ret = ESP_OK;

    if ((session == NULL) || (response == NULL) || (new_tasks == NULL) || (new_task_capacity == 0U)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = device_cloud_service_get_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }
    if (config.pull_tasks_url[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    ret = device_cloud_service_build_device_request_json(request_body, sizeof(request_body));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = device_cloud_session_post_json(session, config.pull_tasks_url, request_body, response);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "pullPlaybackTasks failed: status=%d body=%s",
                 response->status_code,
                 response->buffer == NULL ? "" : response->buffer);
        return ret;
    }

    root = cJSON_Parse(response->buffer);
    if (root == NULL) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    task_array = playback_task_find_array(root);
    if (!cJSON_IsArray(task_array)) {
        ret = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    for (index = 0; index < (size_t)cJSON_GetArraySize(task_array) && new_task_count < new_task_capacity; ++index) {
        playback_task_t parsed_task = {0};
        const cJSON *task_object = cJSON_GetArrayItem(task_array, (int)index);
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
    ret = ESP_OK;

cleanup:
    cJSON_Delete(root);
    return ret;
}

static void playback_task_download_missing_audio(void)
{
    size_t index = 0;
    const char *keep_paths[PLAYBACK_TASK_MAX_COUNT] = {0};
    size_t keep_count = 0;
    bool changed = false;

    if (!playback_task_is_connected() || !audio_cache_service_is_ready()) {
        return;
    }

    for (index = 0; index < s_task_count; ++index) {
        playback_task_t *task = &s_tasks[index];

        if ((task->audio_cached != 0U) && (task->local_audio_path[0] != '\0')) {
            keep_paths[keep_count++] = task->local_audio_path;
        }

        if ((task->audio_url[0] == '\0') || (task->audio_cached != 0U)) {
            continue;
        }

        if (audio_cache_service_download(task->instance_id,
                                         task->audio_url,
                                         task->local_audio_path,
                                         sizeof(task->local_audio_path)) == ESP_OK) {
            task->audio_cached = 1U;
            task->audio_status = PLAYBACK_AUDIO_STATUS_CACHED;
            task->task_status = PLAYBACK_TASK_STATUS_READY;
            if (task->local_audio_path[0] != '\0') {
                keep_paths[keep_count++] = task->local_audio_path;
            }
            changed = true;
            playback_task_enqueue_report_status(task, PLAYBACK_TASK_STATUS_READY);
        } else {
            task->audio_cached = 0U;
            task->audio_status = PLAYBACK_AUDIO_STATUS_FAILED;
            task->local_audio_path[0] = '\0';
            if (task->task_status == PLAYBACK_TASK_STATUS_READY) {
                task->task_status = PLAYBACK_TASK_STATUS_PENDING;
            }
            changed = true;
            playback_task_enqueue_report_status(task, PLAYBACK_TASK_STATUS_FAILED);
        }
    }

    audio_cache_service_cleanup_unused(keep_paths, keep_count);
    if (changed) {
        playback_task_mark_dirty(false);
    }
}

static esp_err_t playback_task_play_now(playback_task_t *task)
{
    const char *play_path = NULL;
    audio_service_format_t format = AUDIO_SERVICE_FORMAT_AUTO;
    esp_err_t ret = ESP_OK;

    if (task == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if ((task->audio_cached != 0U) && (task->local_audio_path[0] != '\0') &&
        audio_cache_service_file_exists(task->local_audio_path)) {
        play_path = task->local_audio_path;
    } else if (playback_task_allows_fallback(task) && storage_service_default_audio_exists()) {
        play_path = storage_service_get_default_audio_path();
        format = AUDIO_SERVICE_FORMAT_WAV;
    } else {
        task->task_status = PLAYBACK_TASK_STATUS_FAILED;
        task->audio_status = PLAYBACK_AUDIO_STATUS_FAILED;
        task->expires_at_epoch = time(NULL) + PLAYBACK_TASK_KEEP_FILE_SECONDS;
        playback_task_mark_dirty(true);
        playback_task_enqueue_report_status(task, PLAYBACK_TASK_STATUS_FAILED);
        return ESP_ERR_NOT_FOUND;
    }

    task->task_status = PLAYBACK_TASK_STATUS_PLAYING;
    playback_task_enqueue_report_status(task, PLAYBACK_TASK_STATUS_PLAYING);

    ret = audio_service_play(play_path, format, 100, 0);
    if ((ret != ESP_OK) &&
        (format == AUDIO_SERVICE_FORMAT_AUTO) &&
        playback_task_allows_fallback(task) &&
        storage_service_default_audio_exists()) {
        task->audio_status = PLAYBACK_AUDIO_STATUS_FAILED;
        ret = audio_service_play(storage_service_get_default_audio_path(),
                                 AUDIO_SERVICE_FORMAT_WAV,
                                 100,
                                 0);
    }

    if (ret == ESP_OK) {
        task->task_status = PLAYBACK_TASK_STATUS_FINISHED;
    } else {
        task->task_status = PLAYBACK_TASK_STATUS_FAILED;
        task->audio_status = PLAYBACK_AUDIO_STATUS_FAILED;
    }

    task->expires_at_epoch = time(NULL) + PLAYBACK_TASK_KEEP_FILE_SECONDS;
    playback_task_mark_dirty(true);
    playback_task_enqueue_report_status(task, (playback_task_status_t)task->task_status);

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
    char *response_buffer = NULL;
    device_cloud_http_response_t response = {0};
    device_cloud_session_t sync_session = {0};
    time_t next_sync_epoch = 0;

    (void)arg;

    response_buffer = calloc(1, PLAYBACK_TASK_RESPONSE_BYTES);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate playback sync response buffer");
        free(response_buffer);
        vTaskDelete(NULL);
        return;
    }

    response.buffer = response_buffer;
    response.buffer_size = PLAYBACK_TASK_RESPONSE_BYTES;
    device_cloud_session_init(&sync_session, response_buffer, PLAYBACK_TASK_RESPONSE_BYTES);

    while (true) {
        device_cloud_config_t config = {0};
        uint32_t notify_bits = 0;
        time_t now = 0;

        xEventGroupWaitBits(s_connected_event_group,
                            s_connected_bit,
                            pdFALSE,
                            pdTRUE,
                            portMAX_DELAY);

        if (device_cloud_service_get_generation() != s_last_config_generation) {
            s_last_config_generation = device_cloud_service_get_generation();
            s_force_sync = true;
        }

        if (!time_service_has_valid_time()) {
            if (xTaskNotifyWait(0, UINT32_MAX, &notify_bits, pdMS_TO_TICKS(PLAYBACK_TASK_TIME_WAIT_MS)) == pdTRUE) {
                if ((notify_bits & PLAYBACK_TASK_NOTIFY_SYNC) != 0U) {
                    s_force_sync = true;
                }
                if ((notify_bits & PLAYBACK_TASK_NOTIFY_SAVE) != 0U) {
                    playback_task_flush_state_if_dirty();
                }
            }
            continue;
        }

        now = time(NULL);
        if (playback_task_process_due_tasks(now)) {
            s_force_sync = true;
        }
        if (playback_task_remove_expired(now)) {
            playback_task_mark_dirty(false);
        }

        if ((device_cloud_service_get_config(&config) == ESP_OK) &&
            playback_task_is_connected() &&
            (s_force_sync || (next_sync_epoch == 0) || (now >= next_sync_epoch))) {
            playback_task_t *scratch_tasks = calloc(PLAYBACK_TASK_MAX_COUNT, sizeof(*scratch_tasks));

            if (scratch_tasks == NULL) {
                ESP_LOGE(TAG, "Failed to allocate playback sync scratch tasks");
            } else {
                if (playback_task_sync_from_cloud(&sync_session,
                                                  &response,
                                                  scratch_tasks,
                                                  PLAYBACK_TASK_MAX_COUNT) == ESP_OK) {
                    playback_task_download_missing_audio();
                }
                free(scratch_tasks);
            }
            now = time(NULL);
            next_sync_epoch = now + (time_t)(config.task_poll_seconds > 0U ? config.task_poll_seconds : (5U * 60U));
            s_force_sync = false;
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
        }
    }
}

esp_err_t playback_task_service_init(void)
{
    playback_task_load_state();
    s_state_dirty = false;
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

    s_report_queue = xQueueCreate(PLAYBACK_TASK_REPORT_QUEUE_LENGTH, sizeof(playback_report_event_t));
    if (s_report_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_save_timer = xTimerCreate("playback_save",
                                pdMS_TO_TICKS(PLAYBACK_TASK_SAVE_DELAY_MS),
                                pdFALSE,
                                NULL,
                                playback_task_save_timer_callback);
    if (s_save_timer == NULL) {
        vQueueDelete(s_report_queue);
        s_report_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(playback_task_report_task,
                    "playback_report",
                    PLAYBACK_TASK_REPORT_TASK_STACK_SIZE,
                    NULL,
                    PLAYBACK_TASK_REPORT_TASK_PRIORITY,
                    &s_report_task_handle) != pdPASS) {
        xTimerDelete(s_save_timer, 0);
        s_save_timer = NULL;
        vQueueDelete(s_report_queue);
        s_report_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(playback_task_service_task,
                    "playback_task",
                    PLAYBACK_TASK_TASK_STACK_SIZE,
                    NULL,
                    PLAYBACK_TASK_TASK_PRIORITY,
                    &s_task_handle) != pdPASS) {
        vTaskDelete(s_report_task_handle);
        s_report_task_handle = NULL;
        xTimerDelete(s_save_timer, 0);
        s_save_timer = NULL;
        vQueueDelete(s_report_queue);
        s_report_queue = NULL;
        return ESP_ERR_NO_MEM;
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
