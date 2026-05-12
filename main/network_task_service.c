#include "network_task_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_cache_service.h"
#include "device_cloud_service.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "NETWORK_TASK";

#define NETWORK_TASK_STACK_SIZE              12288
#define NETWORK_TASK_PRIORITY                4
#define NETWORK_TASK_RESPONSE_BYTES          4096
#define NETWORK_TASK_REPORT_BYTES            512
#define NETWORK_TASK_MAX_REPORT_JOBS         8
#define NETWORK_TASK_MAX_AUDIO_JOBS          12
#define NETWORK_TASK_HTTPS_HEAP_FREE_MIN     30000
#define NETWORK_TASK_HTTPS_HEAP_LARGEST_MIN  15000
#define NETWORK_TASK_NOTIFY_WORK             BIT0
#define NETWORK_TASK_NOTIFY_RESET_SESSION    BIT1

typedef enum {
    NETWORK_TASK_JOB_NONE = 0,
    NETWORK_TASK_JOB_ALARM_REPORT,
    NETWORK_TASK_JOB_ALARM_PULL,
    NETWORK_TASK_JOB_WEATHER_REFRESH,
    NETWORK_TASK_JOB_AUDIO_DOWNLOAD,
    NETWORK_TASK_JOB_LOW_SYNC,
} network_task_job_type_t;

typedef struct {
    bool in_use;
    char instance_id[48];
    char status[24];
    char audio_status[24];
} network_report_job_t;

typedef struct {
    bool in_use;
    char download_url[AUDIO_CACHE_URL_SIZE];
    int64_t ring_at_epoch;
} network_audio_download_job_t;

typedef struct {
    network_task_job_type_t type;
    union {
        network_report_job_t report;
        network_task_playback_reason_t playback_reason;
        network_task_weather_reason_t weather_reason;
        network_audio_download_job_t audio_download;
    } data;
} network_task_job_t;

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_connected_event_group;
static EventBits_t s_connected_bit;
static TaskHandle_t s_task_handle;
static bool s_initialized;
static network_task_playback_reason_t s_pending_playback_reason;
static network_task_weather_reason_t s_pending_weather_reason;
static network_audio_download_job_t s_audio_download_jobs[NETWORK_TASK_MAX_AUDIO_JOBS];
static bool s_audio_cache_cleanup_pending;
static bool s_audio_cache_batch_active;
static esp_err_t s_audio_cache_batch_ret = ESP_OK;
static char s_audio_cache_protected_path[AUDIO_CACHE_PATH_MAX];
static network_report_job_t s_report_jobs[NETWORK_TASK_MAX_REPORT_JOBS];
static network_task_json_result_cb_t s_playback_handler;
static void *s_playback_handler_ctx;
static network_task_json_result_cb_t s_weather_handler;
static void *s_weather_handler_ctx;
static network_task_audio_cache_result_cb_t s_audio_cache_handler;
static void *s_audio_cache_handler_ctx;
static network_task_audio_cache_done_cb_t s_audio_cache_done_handler;
static void *s_audio_cache_done_handler_ctx;
static network_task_startup_pull_done_cb_t s_startup_pull_done_handler;
static void *s_startup_pull_done_handler_ctx;
static network_task_startup_weather_done_cb_t s_startup_weather_done_handler;
static void *s_startup_weather_done_handler_ctx;

static void network_task_copy_string(char *destination, size_t destination_size, const char *source)
{
    if ((destination == NULL) || (destination_size == 0)) {
        return;
    }

    snprintf(destination, destination_size, "%s", source == NULL ? "" : source);
}

static void network_task_log_heap(const char *stage)
{
    ESP_LOGI(TAG,
             "Heap %s: free=%u largest=%u",
             stage == NULL ? "unknown" : stage,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void network_task_notify(void)
{
    if (s_task_handle != NULL) {
        xTaskNotify(s_task_handle, NETWORK_TASK_NOTIFY_WORK, eSetBits);
    }
}

static void network_task_lock(void)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
}

static void network_task_unlock(void)
{
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
}

static bool network_task_https_heap_ready(const char *job_name)
{
    uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    if ((free_heap >= NETWORK_TASK_HTTPS_HEAP_FREE_MIN) &&
        (largest >= NETWORK_TASK_HTTPS_HEAP_LARGEST_MIN)) {
        return true;
    }

    ESP_LOGW(TAG,
             "HTTPS heap insufficient: job=%s free=%u/%u largest=%u/%u",
             job_name == NULL ? "unknown" : job_name,
             (unsigned int)free_heap,
             (unsigned int)NETWORK_TASK_HTTPS_HEAP_FREE_MIN,
             (unsigned int)largest,
             (unsigned int)NETWORK_TASK_HTTPS_HEAP_LARGEST_MIN);
    return false;
}

static const char *network_task_playback_job_name(network_task_playback_reason_t reason)
{
    switch (reason) {
    case NETWORK_TASK_PLAYBACK_REASON_STARTUP:
        return "startup_playback_pull";
    case NETWORK_TASK_PLAYBACK_REASON_POST_PLAYBACK:
        return "post_playback_pull";
    case NETWORK_TASK_PLAYBACK_REASON_NORMAL:
        return "playback_pull";
    default:
        return "playback_pull";
    }
}

static const char *network_task_weather_job_name(network_task_weather_reason_t reason)
{
    switch (reason) {
    case NETWORK_TASK_WEATHER_REASON_STARTUP:
        return "startup_weather_refresh";
    case NETWORK_TASK_WEATHER_REASON_NORMAL:
        return "weather_refresh";
    default:
        return "weather_refresh";
    }
}

static bool network_task_audio_jobs_empty_locked(void)
{
    size_t index = 0;

    for (index = 0; index < NETWORK_TASK_MAX_AUDIO_JOBS; ++index) {
        if (s_audio_download_jobs[index].in_use) {
            return false;
        }
    }

    return true;
}

static bool network_task_download_url_queued_locked(const char *download_url)
{
    size_t index = 0;

    if ((download_url == NULL) || (download_url[0] == '\0')) {
        return true;
    }

    for (index = 0; index < NETWORK_TASK_MAX_AUDIO_JOBS; ++index) {
        if (s_audio_download_jobs[index].in_use &&
            (strcmp(s_audio_download_jobs[index].download_url, download_url) == 0)) {
            return true;
        }
    }

    return false;
}

static bool network_task_take_next_job(network_task_job_t *job)
{
    size_t index = 0;
    size_t best_audio_index = NETWORK_TASK_MAX_AUDIO_JOBS;

    if (job == NULL) {
        return false;
    }

    memset(job, 0, sizeof(*job));

    network_task_lock();
    for (index = 0; index < NETWORK_TASK_MAX_REPORT_JOBS; ++index) {
        if (s_report_jobs[index].in_use) {
            job->type = NETWORK_TASK_JOB_ALARM_REPORT;
            job->data.report = s_report_jobs[index];
            memset(&s_report_jobs[index], 0, sizeof(s_report_jobs[index]));
            network_task_unlock();
            return true;
        }
    }

    if (s_pending_playback_reason != NETWORK_TASK_PLAYBACK_REASON_NONE) {
        job->type = NETWORK_TASK_JOB_ALARM_PULL;
        job->data.playback_reason = s_pending_playback_reason;
        s_pending_playback_reason = NETWORK_TASK_PLAYBACK_REASON_NONE;
        network_task_unlock();
        return true;
    }

    if (s_pending_weather_reason != NETWORK_TASK_WEATHER_REASON_NONE) {
        job->type = NETWORK_TASK_JOB_WEATHER_REFRESH;
        job->data.weather_reason = s_pending_weather_reason;
        s_pending_weather_reason = NETWORK_TASK_WEATHER_REASON_NONE;
        network_task_unlock();
        return true;
    }

    for (index = 0; index < NETWORK_TASK_MAX_AUDIO_JOBS; ++index) {
        if (!s_audio_download_jobs[index].in_use) {
            continue;
        }
        if ((best_audio_index == NETWORK_TASK_MAX_AUDIO_JOBS) ||
            (s_audio_download_jobs[index].ring_at_epoch <
             s_audio_download_jobs[best_audio_index].ring_at_epoch)) {
            best_audio_index = index;
        }
    }
    if (best_audio_index < NETWORK_TASK_MAX_AUDIO_JOBS) {
        job->type = NETWORK_TASK_JOB_AUDIO_DOWNLOAD;
        job->data.audio_download = s_audio_download_jobs[best_audio_index];
        memset(&s_audio_download_jobs[best_audio_index], 0, sizeof(s_audio_download_jobs[best_audio_index]));
        network_task_unlock();
        return true;
    }

    network_task_unlock();
    return false;
}

static bool network_task_has_pending_work(void)
{
    size_t index = 0;
    bool has_work = false;

    network_task_lock();
    has_work = (s_pending_playback_reason != NETWORK_TASK_PLAYBACK_REASON_NONE) ||
               (s_pending_weather_reason != NETWORK_TASK_WEATHER_REASON_NONE);
    for (index = 0; !has_work && (index < NETWORK_TASK_MAX_REPORT_JOBS); ++index) {
        has_work = s_report_jobs[index].in_use;
    }
    for (index = 0; !has_work && (index < NETWORK_TASK_MAX_AUDIO_JOBS); ++index) {
        has_work = s_audio_download_jobs[index].in_use;
    }
    network_task_unlock();

    return has_work;
}

static void network_task_reset_session(device_cloud_session_t *session,
                                       char *response_buffer,
                                       size_t response_buffer_size)
{
    device_cloud_session_deinit(session);
    device_cloud_session_init(session, response_buffer, response_buffer_size);
}

static void network_task_clear_response(device_cloud_http_response_t *response)
{
    if (response == NULL) {
        return;
    }
    response->length = 0;
    response->status_code = 0;
    response->truncated = false;
    if ((response->buffer != NULL) && (response->buffer_size > 0)) {
        response->buffer[0] = '\0';
    }
}

static void network_task_execute_playback_pull(device_cloud_session_t *session,
                                               device_cloud_http_response_t *response,
                                               char *request_body,
                                               size_t request_body_size,
                                               network_task_playback_reason_t reason)
{
    device_cloud_config_t config = {0};
    esp_err_t ret = ESP_OK;
    const char *job_name = network_task_playback_job_name(reason);

    network_task_clear_response(response);

    if (!network_task_https_heap_ready(job_name)) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK) {
        ret = device_cloud_service_get_config(&config);
    }
    if (ret == ESP_OK) {
        ret = device_cloud_service_build_device_request_json(request_body, request_body_size);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "POST %s", config.pull_tasks_url);
        ret = device_cloud_session_post_json(session, config.pull_tasks_url, request_body, response);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "pullPlaybackTasks failed via network queue: status=%d body=%s",
                 response->status_code,
                 response->buffer == NULL ? "" : response->buffer);
        network_task_reset_session(session, response->buffer, response->buffer_size);
    } else {
        network_task_reset_session(session, response->buffer, response->buffer_size);
    }

    if (s_playback_handler != NULL) {
        s_playback_handler(ret,
                           response->buffer == NULL ? "" : response->buffer,
                           response->length,
                           s_playback_handler_ctx);
    }
    network_task_reset_session(session, response->buffer, response->buffer_size);

    if ((reason == NETWORK_TASK_PLAYBACK_REASON_STARTUP) && (s_startup_pull_done_handler != NULL)) {
        s_startup_pull_done_handler(ret, s_startup_pull_done_handler_ctx);
    }
}

static void network_task_execute_weather_refresh(device_cloud_session_t *session,
                                                 device_cloud_http_response_t *response,
                                                 char *request_body,
                                                 size_t request_body_size,
                                                 network_task_weather_reason_t reason)
{
    device_cloud_config_t config = {0};
    esp_err_t ret = ESP_OK;
    const char *job_name = network_task_weather_job_name(reason);

    network_task_clear_response(response);
    if (!network_task_https_heap_ready(job_name)) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK) {
        ret = device_cloud_service_get_config(&config);
    }
    if (ret == ESP_OK) {
        ret = device_cloud_service_build_device_request_json(request_body, request_body_size);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "POST %s", config.display_state_url);
        ret = device_cloud_session_post_json(session, config.display_state_url, request_body, response);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "getCurrentWeather failed via network queue: status=%d body=%s",
                 response->status_code,
                 response->buffer == NULL ? "" : response->buffer);
        network_task_reset_session(session, response->buffer, response->buffer_size);
    } else {
        network_task_reset_session(session, response->buffer, response->buffer_size);
    }

    if (s_weather_handler != NULL) {
        s_weather_handler(ret,
                          response->buffer == NULL ? "" : response->buffer,
                          response->length,
                          s_weather_handler_ctx);
    }

    network_task_reset_session(session, response->buffer, response->buffer_size);

    if ((reason == NETWORK_TASK_WEATHER_REASON_STARTUP) && (s_startup_weather_done_handler != NULL)) {
        s_startup_weather_done_handler(ret, s_startup_weather_done_handler_ctx);
    }
}

static void network_task_audio_cache_result_bridge(const char *download_url,
                                                   esp_err_t ret,
                                                   const char *local_path,
                                                   void *ctx)
{
    (void)ctx;

    if (s_audio_cache_handler != NULL) {
        s_audio_cache_handler(download_url,
                              ret,
                              local_path,
                              s_audio_cache_handler_ctx);
    }
}

static void network_task_finish_audio_download(esp_err_t ret)
{
    bool batch_done = false;
    esp_err_t done_ret = ESP_OK;

    network_task_lock();
    if (s_audio_cache_batch_active) {
        if ((ret != ESP_OK) && (s_audio_cache_batch_ret == ESP_OK)) {
            s_audio_cache_batch_ret = ret;
        }
        if (network_task_audio_jobs_empty_locked()) {
            batch_done = true;
            done_ret = s_audio_cache_batch_ret;
            s_audio_cache_batch_active = false;
            s_audio_cache_batch_ret = ESP_OK;
            s_audio_cache_cleanup_pending = false;
        }
    }
    network_task_unlock();

    if (batch_done && (s_audio_cache_done_handler != NULL)) {
        s_audio_cache_done_handler(done_ret, s_audio_cache_done_handler_ctx);
    }
}

static void network_task_cleanup_audio_cache_if_needed(const network_audio_download_job_t *current_job)
{
    char protected_path[AUDIO_CACHE_PATH_MAX] = {0};
    char keep_paths[NETWORK_TASK_MAX_AUDIO_JOBS + 1U][AUDIO_CACHE_PATH_MAX] = {0};
    const char *keep_path_ptrs[NETWORK_TASK_MAX_AUDIO_JOBS + 1U] = {0};
    size_t index = 0;
    size_t keep_count = 0;
    bool should_cleanup = false;

    network_task_lock();
    should_cleanup = s_audio_cache_cleanup_pending;
    if (should_cleanup) {
        s_audio_cache_cleanup_pending = false;
        network_task_copy_string(protected_path, sizeof(protected_path), s_audio_cache_protected_path);
        if ((current_job != NULL) && current_job->download_url[0] != '\0') {
            if (audio_cache_service_resolve_path(NULL,
                                                 current_job->download_url,
                                                 keep_paths[keep_count],
                                                 sizeof(keep_paths[keep_count])) == ESP_OK) {
                keep_path_ptrs[keep_count] = keep_paths[keep_count];
                ++keep_count;
            }
        }
        for (index = 0; (index < NETWORK_TASK_MAX_AUDIO_JOBS) && (keep_count < (NETWORK_TASK_MAX_AUDIO_JOBS + 1U)); ++index) {
            if (s_audio_download_jobs[index].in_use) {
                if (audio_cache_service_resolve_path(NULL,
                                                     s_audio_download_jobs[index].download_url,
                                                     keep_paths[keep_count],
                                                     sizeof(keep_paths[keep_count])) == ESP_OK) {
                    keep_path_ptrs[keep_count] = keep_paths[keep_count];
                    ++keep_count;
                }
            }
        }
    }
    network_task_unlock();

    if (!should_cleanup) {
        return;
    }

    ESP_LOGI(TAG, "Cleaning audio cache keep=%u protected=%s",
             (unsigned int)keep_count,
             protected_path);
    (void)audio_cache_service_cleanup_unused_protected(keep_path_ptrs, keep_count, protected_path);
}

static void network_task_execute_audio_download(const network_audio_download_job_t *job)
{
    char local_path[AUDIO_CACHE_PATH_MAX] = {0};
    esp_err_t ret = ESP_OK;

    if ((job == NULL) || (job->download_url[0] == '\0')) {
        network_task_finish_audio_download(ESP_ERR_INVALID_ARG);
        return;
    }

    network_task_cleanup_audio_cache_if_needed(job);

    ret = audio_cache_service_resolve_path(NULL, job->download_url, local_path, sizeof(local_path));
    if (ret != ESP_OK) {
        network_task_audio_cache_result_bridge(job->download_url, ret, "", NULL);
        network_task_finish_audio_download(ret);
        return;
    }

    if (audio_cache_service_file_exists(local_path)) {
        ESP_LOGI(TAG, "Audio cache hit via network queue: path=%s", local_path);
        network_task_audio_cache_result_bridge(job->download_url, ESP_OK, local_path, NULL);
        network_task_finish_audio_download(ESP_OK);
        return;
    }

    ESP_LOGI(TAG, "Running audio_download path=%s downloadUrl=%s", local_path, job->download_url);
    network_task_log_heap("before_audio_download");
    ret = audio_cache_service_download(NULL, job->download_url, local_path, sizeof(local_path));
    network_task_log_heap("after_audio_download");
    network_task_audio_cache_result_bridge(job->download_url, ret, local_path, NULL);
    network_task_finish_audio_download(ret);
}

static void network_task_execute_report(device_cloud_session_t *session,
                                        device_cloud_http_response_t *response,
                                        char *request_body,
                                        size_t request_body_size,
                                        const network_report_job_t *job)
{
    device_cloud_config_t config = {0};
    int written = 0;
    esp_err_t ret = ESP_OK;

    if ((job == NULL) || (job->instance_id[0] == '\0') || (job->status[0] == '\0')) {
        return;
    }

    network_task_clear_response(response);
    if (!network_task_https_heap_ready("playback_report")) {
        ret = ESP_ERR_NO_MEM;
    }
    if (ret == ESP_OK) {
        ret = device_cloud_service_get_config(&config);
    }
    if (ret != ESP_OK) {
        return;
    }

    if (job->audio_status[0] != '\0') {
        written = snprintf(request_body,
                           request_body_size,
                           "{\"clientId\":\"%s\",\"deviceId\":\"%s\",\"instanceId\":\"%s\","
                           "\"status\":\"%s\",\"audioStatus\":\"%s\"}",
                           config.client_id,
                           config.device_id,
                           job->instance_id,
                           job->status,
                           job->audio_status);
    } else {
        written = snprintf(request_body,
                           request_body_size,
                           "{\"clientId\":\"%s\",\"deviceId\":\"%s\",\"instanceId\":\"%s\","
                           "\"status\":\"%s\"}",
                           config.client_id,
                           config.device_id,
                           job->instance_id,
                           job->status);
    }

    if ((written < 0) || ((size_t)written >= request_body_size)) {
        ESP_LOGW(TAG, "Playback report body too large for %s", job->instance_id);
        return;
    }

    ESP_LOGI(TAG, "POST %s", config.report_status_url);
    ret = device_cloud_session_post_json(session, config.report_status_url, request_body, response);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Reported %s for %s", job->status, job->instance_id);
    } else {
        ESP_LOGW(TAG,
                 "Playback report failed for %s status=%s: %s",
                 job->instance_id,
                 job->status,
                 esp_err_to_name(ret));
        network_task_reset_session(session, response->buffer, response->buffer_size);
    }

    network_task_reset_session(session, response->buffer, response->buffer_size);
}

static void network_task_worker(void *arg)
{
    char *response_buffer = NULL;
    char request_body[NETWORK_TASK_REPORT_BYTES] = {0};
    device_cloud_http_response_t response = {0};
    device_cloud_session_t session = {0};

    (void)arg;

    response_buffer = calloc(1, NETWORK_TASK_RESPONSE_BYTES);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate network response buffer");
        vTaskDelete(NULL);
        return;
    }

    response.buffer = response_buffer;
    response.buffer_size = NETWORK_TASK_RESPONSE_BYTES;
    device_cloud_session_init(&session, response_buffer, NETWORK_TASK_RESPONSE_BYTES);
    ESP_LOGI(TAG,
             "network_task stack_free=%u bytes",
             (unsigned int)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    while (true) {
        uint32_t notify_bits = 0;
        network_task_job_t job = {0};

        xEventGroupWaitBits(s_connected_event_group,
                            s_connected_bit,
                            pdFALSE,
                            pdTRUE,
                            portMAX_DELAY);

        if (!network_task_has_pending_work()) {
            xTaskNotifyWait(0, UINT32_MAX, &notify_bits, portMAX_DELAY);
        } else {
            xTaskNotifyWait(0, UINT32_MAX, &notify_bits, 0);
        }

        if ((notify_bits & NETWORK_TASK_NOTIFY_RESET_SESSION) != 0U) {
            network_task_reset_session(&session, response_buffer, NETWORK_TASK_RESPONSE_BYTES);
        }

        if (!network_task_take_next_job(&job)) {
            continue;
        }

        switch (job.type) {
        case NETWORK_TASK_JOB_ALARM_REPORT:
            network_task_execute_report(&session,
                                        &response,
                                        request_body,
                                        sizeof(request_body),
                                        &job.data.report);
            break;
        case NETWORK_TASK_JOB_ALARM_PULL:
            network_task_execute_playback_pull(&session,
                                               &response,
                                               request_body,
                                               sizeof(request_body),
                                               job.data.playback_reason);
            break;
        case NETWORK_TASK_JOB_WEATHER_REFRESH:
            network_task_execute_weather_refresh(&session,
                                                 &response,
                                                 request_body,
                                                 sizeof(request_body),
                                                 job.data.weather_reason);
            break;
        case NETWORK_TASK_JOB_AUDIO_DOWNLOAD:
            network_task_reset_session(&session, response_buffer, NETWORK_TASK_RESPONSE_BYTES);
            network_task_execute_audio_download(&job.data.audio_download);
            break;
        case NETWORK_TASK_JOB_LOW_SYNC:
        case NETWORK_TASK_JOB_NONE:
        default:
            break;
        }
    }
}

esp_err_t network_task_service_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    s_initialized = true;
    return ESP_OK;
}

esp_err_t network_task_service_start(EventGroupHandle_t connected_event_group, EventBits_t connected_bit)
{
    if ((connected_event_group == NULL) || (connected_bit == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_handle != NULL) {
        return ESP_OK;
    }

    s_connected_event_group = connected_event_group;
    s_connected_bit = connected_bit;

    if (xTaskCreate(network_task_worker,
                    "network_task",
                    NETWORK_TASK_STACK_SIZE,
                    NULL,
                    NETWORK_TASK_PRIORITY,
                    &s_task_handle) != pdPASS) {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Network task service started");
    return ESP_OK;
}

void network_task_service_register_playback_pull_handler(network_task_json_result_cb_t handler, void *ctx)
{
    s_playback_handler = handler;
    s_playback_handler_ctx = ctx;
}

void network_task_service_register_weather_handler(network_task_json_result_cb_t handler, void *ctx)
{
    s_weather_handler = handler;
    s_weather_handler_ctx = ctx;
}

void network_task_service_register_audio_cache_handler(network_task_audio_cache_result_cb_t handler, void *ctx)
{
    s_audio_cache_handler = handler;
    s_audio_cache_handler_ctx = ctx;
}

void network_task_service_register_audio_cache_done_handler(network_task_audio_cache_done_cb_t handler, void *ctx)
{
    s_audio_cache_done_handler = handler;
    s_audio_cache_done_handler_ctx = ctx;
}

void network_task_service_register_startup_pull_done_handler(network_task_startup_pull_done_cb_t handler, void *ctx)
{
    s_startup_pull_done_handler = handler;
    s_startup_pull_done_handler_ctx = ctx;
}

void network_task_service_register_startup_weather_done_handler(network_task_startup_weather_done_cb_t handler,
                                                                void *ctx)
{
    s_startup_weather_done_handler = handler;
    s_startup_weather_done_handler_ctx = ctx;
}

void network_task_service_request_playback_pull(network_task_playback_reason_t reason)
{
    if (reason == NETWORK_TASK_PLAYBACK_REASON_NONE) {
        reason = NETWORK_TASK_PLAYBACK_REASON_NORMAL;
    }

    network_task_lock();
    if (reason == NETWORK_TASK_PLAYBACK_REASON_POST_PLAYBACK) {
        s_pending_playback_reason = reason;
    } else if ((reason == NETWORK_TASK_PLAYBACK_REASON_STARTUP) &&
               (s_pending_playback_reason != NETWORK_TASK_PLAYBACK_REASON_POST_PLAYBACK)) {
        s_pending_playback_reason = reason;
    } else if (s_pending_playback_reason == NETWORK_TASK_PLAYBACK_REASON_NONE) {
        s_pending_playback_reason = reason;
    }
    network_task_unlock();

    ESP_LOGI(TAG, "Queued %s", network_task_playback_job_name(reason));
    network_task_notify();
}

void network_task_service_request_weather_refresh(network_task_weather_reason_t reason)
{
    bool queued = false;

    if (reason == NETWORK_TASK_WEATHER_REASON_NONE) {
        reason = NETWORK_TASK_WEATHER_REASON_NORMAL;
    }

    network_task_lock();
    if (reason == NETWORK_TASK_WEATHER_REASON_STARTUP) {
        s_pending_weather_reason = reason;
        queued = true;
    } else if (s_pending_weather_reason != NETWORK_TASK_WEATHER_REASON_STARTUP) {
        s_pending_weather_reason = reason;
        queued = true;
    }
    network_task_unlock();

    if (queued) {
        ESP_LOGI(TAG, "Queued %s", network_task_weather_job_name(reason));
        network_task_notify();
    } else {
        ESP_LOGI(TAG, "Skipped weather_refresh because startup_weather_refresh is pending");
    }
}

void network_task_service_request_audio_cache_maintenance(const network_task_audio_cache_item_t *items,
                                                          size_t item_count,
                                                          const char *protected_path)
{
    size_t index = 0;
    size_t queued_count = 0;
    size_t dropped_count = 0;

    if ((items == NULL) || (item_count == 0)) {
        return;
    }

    network_task_lock();
    memset(s_audio_download_jobs, 0, sizeof(s_audio_download_jobs));
    network_task_copy_string(s_audio_cache_protected_path,
                             sizeof(s_audio_cache_protected_path),
                             protected_path);
    s_audio_cache_batch_active = true;
    s_audio_cache_batch_ret = ESP_OK;
    s_audio_cache_cleanup_pending = true;

    for (index = 0; index < item_count; ++index) {
        size_t slot = 0;

        if (items[index].download_url[0] == '\0') {
            continue;
        }
        if (network_task_download_url_queued_locked(items[index].download_url)) {
            continue;
        }
        for (slot = 0; slot < NETWORK_TASK_MAX_AUDIO_JOBS; ++slot) {
            if (!s_audio_download_jobs[slot].in_use) {
                s_audio_download_jobs[slot].in_use = true;
                network_task_copy_string(s_audio_download_jobs[slot].download_url,
                                         sizeof(s_audio_download_jobs[slot].download_url),
                                         items[index].download_url);
                s_audio_download_jobs[slot].ring_at_epoch = items[index].ring_at_epoch;
                ++queued_count;
                break;
            }
        }
        if (slot >= NETWORK_TASK_MAX_AUDIO_JOBS) {
            ++dropped_count;
        }
    }

    if (queued_count == 0U) {
        s_audio_cache_batch_active = false;
        s_audio_cache_cleanup_pending = false;
    }
    network_task_unlock();

    ESP_LOGI(TAG,
             "Queued audio_download jobs=%u dropped=%u",
             (unsigned int)queued_count,
             (unsigned int)dropped_count);
    if (queued_count > 0U) {
        network_task_notify();
    } else if (s_audio_cache_done_handler != NULL) {
        s_audio_cache_done_handler(ESP_OK, s_audio_cache_done_handler_ctx);
    }
}

void network_task_service_request_playback_report(const char *instance_id,
                                                  const char *status,
                                                  const char *audio_status)
{
    size_t index = 0;
    size_t free_index = NETWORK_TASK_MAX_REPORT_JOBS;

    if ((instance_id == NULL) || (instance_id[0] == '\0') ||
        (status == NULL) || (status[0] == '\0')) {
        return;
    }

    network_task_lock();
    for (index = 0; index < NETWORK_TASK_MAX_REPORT_JOBS; ++index) {
        if (s_report_jobs[index].in_use &&
            (strcmp(s_report_jobs[index].instance_id, instance_id) == 0) &&
            (strcmp(s_report_jobs[index].status, status) == 0)) {
            network_task_copy_string(s_report_jobs[index].audio_status,
                                     sizeof(s_report_jobs[index].audio_status),
                                     audio_status);
            network_task_unlock();
            ESP_LOGI(TAG, "Replaced playback_report for %s status=%s", instance_id, status);
            network_task_notify();
            return;
        }
        if (!s_report_jobs[index].in_use && (free_index == NETWORK_TASK_MAX_REPORT_JOBS)) {
            free_index = index;
        }
    }
    if (free_index < NETWORK_TASK_MAX_REPORT_JOBS) {
        s_report_jobs[free_index].in_use = true;
        network_task_copy_string(s_report_jobs[free_index].instance_id,
                                 sizeof(s_report_jobs[free_index].instance_id),
                                 instance_id);
        network_task_copy_string(s_report_jobs[free_index].status,
                                 sizeof(s_report_jobs[free_index].status),
                                 status);
        network_task_copy_string(s_report_jobs[free_index].audio_status,
                                 sizeof(s_report_jobs[free_index].audio_status),
                                 audio_status);
    }
    network_task_unlock();

    if (free_index >= NETWORK_TASK_MAX_REPORT_JOBS) {
        ESP_LOGW(TAG, "Playback report queue full, dropping %s status=%s", instance_id, status);
        return;
    }

    ESP_LOGI(TAG, "Queued playback_report for %s status=%s", instance_id, status);
    network_task_notify();
}

void network_task_service_reset_sessions(void)
{
    if (s_task_handle != NULL) {
        xTaskNotify(s_task_handle, NETWORK_TASK_NOTIFY_RESET_SESSION, eSetBits);
    }
}
