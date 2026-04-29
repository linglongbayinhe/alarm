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

#define NETWORK_TASK_STACK_SIZE              14336
#define NETWORK_TASK_PRIORITY                4
#define NETWORK_TASK_RESPONSE_BYTES          4096
#define NETWORK_TASK_REPORT_BYTES            512
#define NETWORK_TASK_MAX_AUDIO_JOBS          8
#define NETWORK_TASK_MAX_REPORT_JOBS         8
#define NETWORK_TASK_HEAP_FREE_MIN           50000
#define NETWORK_TASK_HEAP_LARGEST_MIN        28000
#define NETWORK_TASK_HEAP_STABLE_COUNT       2
#define NETWORK_TASK_HEAP_CHECK_INTERVAL_MS  2000
#define NETWORK_TASK_POST_PLAY_MAX_WAIT_MS   90000
#define NETWORK_TASK_NOTIFY_WORK             BIT0
#define NETWORK_TASK_NOTIFY_RESET_SESSION    BIT1

typedef struct {
    bool in_use;
    char instance_id[48];
    char audio_url[256];
} network_audio_job_t;

typedef struct {
    bool in_use;
    char instance_id[48];
    char status[24];
    char audio_status[24];
} network_report_job_t;

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_connected_event_group;
static EventBits_t s_connected_bit;
static TaskHandle_t s_task_handle;
static bool s_initialized;
static bool s_pending_post_playback_pull;
static bool s_pending_playback_pull;
static bool s_pending_weather_refresh;
static network_audio_job_t s_audio_jobs[NETWORK_TASK_MAX_AUDIO_JOBS];
static network_report_job_t s_report_jobs[NETWORK_TASK_MAX_REPORT_JOBS];
static network_task_json_result_cb_t s_playback_handler;
static void *s_playback_handler_ctx;
static network_task_json_result_cb_t s_weather_handler;
static void *s_weather_handler_ctx;
static network_task_audio_result_cb_t s_audio_handler;
static void *s_audio_handler_ctx;

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

static bool network_task_wait_for_post_playback_heap(void)
{
    uint32_t stable_count = 0;
    uint32_t waited_ms = 0;

    while (waited_ms <= NETWORK_TASK_POST_PLAY_MAX_WAIT_MS) {
        uint32_t free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        uint32_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

        if ((free_heap >= NETWORK_TASK_HEAP_FREE_MIN) &&
            (largest >= NETWORK_TASK_HEAP_LARGEST_MIN)) {
            ++stable_count;
            if (stable_count >= NETWORK_TASK_HEAP_STABLE_COUNT) {
                network_task_log_heap("post_playback_ready");
                return true;
            }
        } else {
            stable_count = 0;
            ESP_LOGI(TAG,
                     "Waiting for post playback heap: free=%u/%u largest=%u/%u",
                     (unsigned int)free_heap,
                     (unsigned int)NETWORK_TASK_HEAP_FREE_MIN,
                     (unsigned int)largest,
                     (unsigned int)NETWORK_TASK_HEAP_LARGEST_MIN);
        }

        vTaskDelay(pdMS_TO_TICKS(NETWORK_TASK_HEAP_CHECK_INTERVAL_MS));
        waited_ms += NETWORK_TASK_HEAP_CHECK_INTERVAL_MS;
    }

    network_task_log_heap("post_playback_heap_timeout");
    ESP_LOGW(TAG, "Post playback heap wait timed out; attempting cloud sync once");
    return false;
}

static bool network_task_take_playback_pull(bool *post_playback)
{
    bool has_job = false;

    network_task_lock();
    if (s_pending_post_playback_pull) {
        s_pending_post_playback_pull = false;
        s_pending_playback_pull = false;
        *post_playback = true;
        has_job = true;
    } else if (s_pending_playback_pull) {
        s_pending_playback_pull = false;
        *post_playback = false;
        has_job = true;
    }
    network_task_unlock();

    return has_job;
}

static bool network_task_take_audio_job(network_audio_job_t *job)
{
    size_t index = 0;
    bool has_job = false;

    if (job == NULL) {
        return false;
    }

    network_task_lock();
    for (index = 0; index < NETWORK_TASK_MAX_AUDIO_JOBS; ++index) {
        if (s_audio_jobs[index].in_use) {
            *job = s_audio_jobs[index];
            memset(&s_audio_jobs[index], 0, sizeof(s_audio_jobs[index]));
            has_job = true;
            break;
        }
    }
    network_task_unlock();

    return has_job;
}

static bool network_task_take_report_job(network_report_job_t *job)
{
    size_t index = 0;
    bool has_job = false;

    if (job == NULL) {
        return false;
    }

    network_task_lock();
    for (index = 0; index < NETWORK_TASK_MAX_REPORT_JOBS; ++index) {
        if (s_report_jobs[index].in_use) {
            *job = s_report_jobs[index];
            memset(&s_report_jobs[index], 0, sizeof(s_report_jobs[index]));
            has_job = true;
            break;
        }
    }
    network_task_unlock();

    return has_job;
}

static bool network_task_take_weather_refresh(void)
{
    bool has_job = false;

    network_task_lock();
    if (s_pending_weather_refresh) {
        s_pending_weather_refresh = false;
        has_job = true;
    }
    network_task_unlock();

    return has_job;
}

static bool network_task_has_pending_work(void)
{
    size_t index = 0;
    bool has_work = false;

    network_task_lock();
    has_work = s_pending_post_playback_pull || s_pending_playback_pull || s_pending_weather_refresh;
    for (index = 0; !has_work && (index < NETWORK_TASK_MAX_AUDIO_JOBS); ++index) {
        has_work = s_audio_jobs[index].in_use;
    }
    for (index = 0; !has_work && (index < NETWORK_TASK_MAX_REPORT_JOBS); ++index) {
        has_work = s_report_jobs[index].in_use;
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
                                               bool post_playback)
{
    device_cloud_config_t config = {0};
    esp_err_t ret = ESP_OK;

    if (post_playback) {
        ESP_LOGI(TAG, "post_playback_pull pending");
        (void)network_task_wait_for_post_playback_heap();
    }
    network_task_clear_response(response);

    ret = device_cloud_service_get_config(&config);
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
    }

    if (s_playback_handler != NULL) {
        s_playback_handler(ret,
                           response->buffer == NULL ? "" : response->buffer,
                           response->length,
                           s_playback_handler_ctx);
    }

    network_task_reset_session(session, response->buffer, response->buffer_size);
}

static void network_task_execute_weather_refresh(device_cloud_session_t *session,
                                                 device_cloud_http_response_t *response,
                                                 char *request_body,
                                                 size_t request_body_size)
{
    device_cloud_config_t config = {0};
    esp_err_t ret = ESP_OK;

    network_task_clear_response(response);
    ret = device_cloud_service_get_config(&config);
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
    }

    if (s_weather_handler != NULL) {
        s_weather_handler(ret,
                          response->buffer == NULL ? "" : response->buffer,
                          response->length,
                          s_weather_handler_ctx);
    }

    network_task_reset_session(session, response->buffer, response->buffer_size);
}

static void network_task_execute_audio_download(const network_audio_job_t *job)
{
    char local_path[96] = {0};
    esp_err_t ret = ESP_OK;

    if ((job == NULL) || (job->instance_id[0] == '\0') || (job->audio_url[0] == '\0')) {
        return;
    }

    ESP_LOGI(TAG, "Downloading audio for %s", job->instance_id);
    network_task_log_heap("before_audio_download");
    ret = audio_cache_service_download(job->instance_id,
                                       job->audio_url,
                                       local_path,
                                       sizeof(local_path));
    network_task_log_heap("after_audio_download");

    if (s_audio_handler != NULL) {
        s_audio_handler(job->instance_id,
                        ret,
                        local_path,
                        s_audio_handler_ctx);
    }
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
    ret = device_cloud_service_get_config(&config);
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

    while (true) {
        uint32_t notify_bits = 0;
        bool post_playback = false;
        network_audio_job_t audio_job = {0};
        network_report_job_t report_job = {0};

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

        if (network_task_take_playback_pull(&post_playback)) {
            network_task_execute_playback_pull(&session,
                                               &response,
                                               request_body,
                                               sizeof(request_body),
                                               post_playback);
            continue;
        }
        if (network_task_take_audio_job(&audio_job)) {
            network_task_execute_audio_download(&audio_job);
            continue;
        }
        if (network_task_take_report_job(&report_job)) {
            network_task_execute_report(&session,
                                        &response,
                                        request_body,
                                        sizeof(request_body),
                                        &report_job);
            continue;
        }
        if (network_task_take_weather_refresh()) {
            network_task_execute_weather_refresh(&session,
                                                 &response,
                                                 request_body,
                                                 sizeof(request_body));
            continue;
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

void network_task_service_register_audio_download_handler(network_task_audio_result_cb_t handler, void *ctx)
{
    s_audio_handler = handler;
    s_audio_handler_ctx = ctx;
}

void network_task_service_request_playback_pull(bool post_playback)
{
    network_task_lock();
    if (post_playback) {
        s_pending_post_playback_pull = true;
        s_pending_playback_pull = false;
    } else if (!s_pending_post_playback_pull) {
        s_pending_playback_pull = true;
    }
    network_task_unlock();

    ESP_LOGI(TAG, "Queued %s", post_playback ? "post_playback_pull" : "playback_pull");
    network_task_notify();
}

void network_task_service_request_weather_refresh(void)
{
    network_task_lock();
    s_pending_weather_refresh = true;
    network_task_unlock();

    ESP_LOGI(TAG, "Queued weather_refresh");
    network_task_notify();
}

void network_task_service_request_audio_download(const char *instance_id, const char *audio_url)
{
    size_t index = 0;
    size_t free_index = NETWORK_TASK_MAX_AUDIO_JOBS;

    if ((instance_id == NULL) || (instance_id[0] == '\0') ||
        (audio_url == NULL) || (audio_url[0] == '\0')) {
        return;
    }

    network_task_lock();
    for (index = 0; index < NETWORK_TASK_MAX_AUDIO_JOBS; ++index) {
        if (s_audio_jobs[index].in_use &&
            (strcmp(s_audio_jobs[index].instance_id, instance_id) == 0)) {
            network_task_copy_string(s_audio_jobs[index].audio_url,
                                     sizeof(s_audio_jobs[index].audio_url),
                                     audio_url);
            network_task_unlock();
            ESP_LOGI(TAG, "Replaced audio_download for %s", instance_id);
            network_task_notify();
            return;
        }
        if (!s_audio_jobs[index].in_use && (free_index == NETWORK_TASK_MAX_AUDIO_JOBS)) {
            free_index = index;
        }
    }
    if (free_index < NETWORK_TASK_MAX_AUDIO_JOBS) {
        s_audio_jobs[free_index].in_use = true;
        network_task_copy_string(s_audio_jobs[free_index].instance_id,
                                 sizeof(s_audio_jobs[free_index].instance_id),
                                 instance_id);
        network_task_copy_string(s_audio_jobs[free_index].audio_url,
                                 sizeof(s_audio_jobs[free_index].audio_url),
                                 audio_url);
    }
    network_task_unlock();

    if (free_index >= NETWORK_TASK_MAX_AUDIO_JOBS) {
        ESP_LOGW(TAG, "Audio download queue full, dropping %s", instance_id);
        return;
    }

    ESP_LOGI(TAG, "Queued audio_download for %s", instance_id);
    network_task_notify();
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
