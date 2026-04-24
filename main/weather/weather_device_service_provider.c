#include "weather_device_service_provider.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "device_cloud_service.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "time_service.h"

#define WEATHER_DEVICE_RESPONSE_BYTES              1536
#define WEATHER_DEVICE_TASK_STACK_SIZE             8192
#define WEATHER_DEVICE_TASK_PRIORITY               4
#define WEATHER_DEVICE_TIME_WAIT_MS                5000
#define WEATHER_DEVICE_TIME_LOG_MS                 30000
#define WEATHER_DEVICE_NOTIFY_REFRESH              BIT0
#define WEATHER_DEVICE_RETRY_FIRST_SECONDS         (30 * 60)
#define WEATHER_DEVICE_RETRY_MAX_SECONDS           (60 * 60)

static const char *TAG = "WEATHER_DEVICE";

static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_snapshot_t s_latest_snapshot;
static bool s_has_snapshot;
static bool s_has_successful_snapshot;
static EventGroupHandle_t s_connected_event_group;
static EventBits_t s_connected_bit;
static TaskHandle_t s_task_handle;
static uint32_t s_last_cloud_generation;

static void weather_device_copy_text(char *destination, size_t destination_size, const char *source)
{
    if ((destination == NULL) || (destination_size == 0)) {
        return;
    }

    snprintf(destination, destination_size, "%s", source == NULL ? "" : source);
}

static bool weather_device_string_equals(const char *left, const char *right)
{
    if ((left == NULL) || (right == NULL)) {
        return false;
    }

    while ((*left != '\0') && (*right != '\0')) {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }

    return (*left == '\0') && (*right == '\0');
}

static weather_condition_t weather_device_map_icon(const char *weather_icon)
{
    if (weather_device_string_equals(weather_icon, "sunny") ||
        weather_device_string_equals(weather_icon, "clear") ||
        weather_device_string_equals(weather_icon, "clear_day") ||
        weather_device_string_equals(weather_icon, "clear-day")) {
        return WEATHER_CONDITION_CLEAR_DAY;
    }
    if (weather_device_string_equals(weather_icon, "clear_night") ||
        weather_device_string_equals(weather_icon, "clear-night")) {
        return WEATHER_CONDITION_CLEAR_NIGHT;
    }
    if (weather_device_string_equals(weather_icon, "cloudy") ||
        weather_device_string_equals(weather_icon, "cloudy_day") ||
        weather_device_string_equals(weather_icon, "cloudy-day")) {
        return WEATHER_CONDITION_CLOUDY_DAY;
    }
    if (weather_device_string_equals(weather_icon, "cloudy_night") ||
        weather_device_string_equals(weather_icon, "cloudy-night")) {
        return WEATHER_CONDITION_CLOUDY_NIGHT;
    }
    if (weather_device_string_equals(weather_icon, "overcast")) {
        return WEATHER_CONDITION_OVERCAST;
    }
    if (weather_device_string_equals(weather_icon, "light_rain") ||
        weather_device_string_equals(weather_icon, "light-rain")) {
        return WEATHER_CONDITION_LIGHT_RAIN;
    }
    if (weather_device_string_equals(weather_icon, "rain") ||
        weather_device_string_equals(weather_icon, "moderate_rain") ||
        weather_device_string_equals(weather_icon, "moderate-rain")) {
        return WEATHER_CONDITION_MODERATE_RAIN;
    }
    if (weather_device_string_equals(weather_icon, "heavy_rain") ||
        weather_device_string_equals(weather_icon, "heavy-rain")) {
        return WEATHER_CONDITION_HEAVY_RAIN;
    }
    if (weather_device_string_equals(weather_icon, "shower")) {
        return WEATHER_CONDITION_SHOWER;
    }
    if (weather_device_string_equals(weather_icon, "thunderstorm")) {
        return WEATHER_CONDITION_THUNDERSTORM;
    }
    if (weather_device_string_equals(weather_icon, "snow")) {
        return WEATHER_CONDITION_SNOW;
    }
    if (weather_device_string_equals(weather_icon, "fog")) {
        return WEATHER_CONDITION_FOG;
    }
    if (weather_device_string_equals(weather_icon, "haze")) {
        return WEATHER_CONDITION_HAZE;
    }
    if (weather_device_string_equals(weather_icon, "dust_storm") ||
        weather_device_string_equals(weather_icon, "dust-storm")) {
        return WEATHER_CONDITION_DUST_STORM;
    }
    if (weather_device_string_equals(weather_icon, "windy")) {
        return WEATHER_CONDITION_WINDY;
    }

    return WEATHER_CONDITION_UNKNOWN;
}

static void weather_device_publish_snapshot(const weather_snapshot_t *snapshot, bool successful)
{
    if (snapshot == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    s_latest_snapshot = *snapshot;
    s_has_snapshot = true;
    if (successful) {
        s_has_successful_snapshot = true;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);
}

static bool weather_device_has_successful_snapshot(void)
{
    bool has_successful_snapshot = false;

    taskENTER_CRITICAL(&s_snapshot_lock);
    has_successful_snapshot = s_has_successful_snapshot;
    taskEXIT_CRITICAL(&s_snapshot_lock);

    return has_successful_snapshot;
}

static void weather_device_publish_simple_state(weather_data_state_t state)
{
    weather_snapshot_t snapshot = {
        .state = state,
        .condition = WEATHER_CONDITION_UNKNOWN,
        .is_daytime = true,
    };

    weather_device_publish_snapshot(&snapshot, false);
}

static esp_err_t weather_device_parse_response(const char *json, weather_snapshot_t *snapshot)
{
    esp_err_t ret = ESP_FAIL;
    cJSON *root = NULL;
    const cJSON *success = NULL;
    const cJSON *temperature = NULL;
    const cJSON *weather_icon = NULL;
    const cJSON *weather_text = NULL;
    const cJSON *target_volume = NULL;
    const cJSON *current_volume = NULL;
    const cJSON *volume_level = NULL;
    weather_condition_t condition = WEATHER_CONDITION_UNKNOWN;

    if ((json == NULL) || (snapshot == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse device display JSON");
        return ESP_FAIL;
    }

    success = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (cJSON_IsBool(success) && !cJSON_IsTrue(success)) {
        ESP_LOGE(TAG, "Device service returned success=false");
        goto cleanup;
    }

    temperature = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    weather_icon = cJSON_GetObjectItemCaseSensitive(root, "weatherIcon");
    weather_text = cJSON_GetObjectItemCaseSensitive(root, "weatherText");
    target_volume = cJSON_GetObjectItemCaseSensitive(root, "targetVolume");
    current_volume = cJSON_GetObjectItemCaseSensitive(root, "currentVolume");
    volume_level = cJSON_GetObjectItemCaseSensitive(root, "volumeLevel");

    if (!cJSON_IsNumber(temperature)) {
        ESP_LOGE(TAG, "Device service JSON missing numeric temperature");
        goto cleanup;
    }
    if (!cJSON_IsString(weather_icon) || (weather_icon->valuestring == NULL)) {
        ESP_LOGE(TAG, "Device service JSON missing string weatherIcon");
        goto cleanup;
    }

    condition = weather_device_map_icon(weather_icon->valuestring);

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->state = WEATHER_DATA_STATE_READY;
    snapshot->condition = condition;
    snapshot->is_daytime = (condition != WEATHER_CONDITION_CLEAR_NIGHT) &&
                           (condition != WEATHER_CONDITION_CLOUDY_NIGHT);
    snapshot->has_current_temperature = true;
    snapshot->current_temperature_c = (int16_t)temperature->valueint;
    snapshot->has_update_time = true;
    snapshot->updated_at_utc = time(NULL);
    snapshot->has_weather_icon_text = true;
    weather_device_copy_text(snapshot->weather_icon_text,
                             sizeof(snapshot->weather_icon_text),
                             weather_icon->valuestring);

    if (cJSON_IsString(weather_text) && (weather_text->valuestring != NULL)) {
        snapshot->has_weather_text = true;
        weather_device_copy_text(snapshot->weather_text,
                                 sizeof(snapshot->weather_text),
                                 weather_text->valuestring);
    }
    if (cJSON_IsNumber(target_volume)) {
        snapshot->has_target_volume = true;
        snapshot->target_volume = (int16_t)target_volume->valueint;
    }
    if (cJSON_IsNumber(current_volume)) {
        snapshot->has_current_volume = true;
        snapshot->current_volume = (int16_t)current_volume->valueint;
    }
    if (cJSON_IsNumber(volume_level)) {
        snapshot->has_volume_level = true;
        snapshot->volume_level = (int16_t)volume_level->valueint;
    }

    ESP_LOGI(TAG,
             "Parsed display state: temperature=%d, weatherIcon=%s, weatherText=%s, volume=%d",
             (int)snapshot->current_temperature_c,
             snapshot->weather_icon_text,
             snapshot->has_weather_text ? snapshot->weather_text : "",
             snapshot->has_volume_level ? (int)snapshot->volume_level : -1);
    ret = ESP_OK;

cleanup:
    cJSON_Delete(root);
    return ret;
}

static esp_err_t weather_device_fetch_once(device_cloud_session_t *session,
                                           device_cloud_http_response_t *response,
                                           char *request_body,
                                           size_t request_body_size)
{
    device_cloud_config_t cloud_config = {0};
    esp_err_t ret = ESP_OK;
    weather_snapshot_t snapshot = {0};

    if ((session == NULL) || (response == NULL) || (request_body == NULL) || (request_body_size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = device_cloud_service_get_config(&cloud_config);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = device_cloud_service_build_device_request_json(request_body, request_body_size);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "POST %s", cloud_config.display_state_url);
    ret = device_cloud_session_post_json(session, cloud_config.display_state_url, request_body, response);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Device display request failed: status=%d body=%s",
                 response->status_code,
                 response->buffer == NULL ? "" : response->buffer);
        return ret;
    }

    if (response->truncated) {
        ESP_LOGW(TAG, "Device service response truncated to %u bytes",
                 (unsigned int)(response->buffer_size - 1));
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Raw JSON: %s", response->buffer == NULL ? "" : response->buffer);

    ret = weather_device_parse_response(response->buffer, &snapshot);
    if (ret == ESP_OK) {
        weather_device_publish_snapshot(&snapshot, true);
    }

    return ret;
}

static uint32_t weather_device_next_retry_seconds(uint32_t failure_count,
                                                  const device_cloud_config_t *config)
{
    if ((config != NULL) && (failure_count == 0U)) {
        return config->display_poll_seconds > 0U
                   ? config->display_poll_seconds
                   : (15U * 60U);
    }
    if (failure_count <= 1U) {
        return WEATHER_DEVICE_RETRY_FIRST_SECONDS;
    }

    return WEATHER_DEVICE_RETRY_MAX_SECONDS;
}

static TickType_t weather_device_wait_for_time_or_refresh(bool *refresh_requested)
{
    int waited_ms = 0;

    while (!time_service_has_valid_time()) {
        uint32_t notify_bits = 0;
        EventBits_t bits = xEventGroupGetBits(s_connected_event_group);

        if ((bits & s_connected_bit) == 0) {
            ESP_LOGW(TAG, "Wi-Fi disconnected while waiting for valid time");
            return portMAX_DELAY;
        }

        if ((waited_ms % WEATHER_DEVICE_TIME_LOG_MS) == 0) {
            ESP_LOGI(TAG, "Waiting for valid system time before HTTPS request");
        }

        weather_device_publish_simple_state(WEATHER_DATA_STATE_LOADING);
        xTaskNotifyWait(0, UINT32_MAX, &notify_bits, pdMS_TO_TICKS(WEATHER_DEVICE_TIME_WAIT_MS));
        if ((notify_bits & WEATHER_DEVICE_NOTIFY_REFRESH) != 0U) {
            *refresh_requested = true;
        }
        waited_ms += WEATHER_DEVICE_TIME_WAIT_MS;
    }

    if (waited_ms > 0) {
        ESP_LOGI(TAG, "System time is valid; starting weather HTTPS request");
        *refresh_requested = true;
    }

    return 0;
}

static void weather_device_provider_task(void *arg)
{
    char request_body[256] = {0};
    char *response_buffer = NULL;
    device_cloud_http_response_t response = {0};
    device_cloud_session_t session = {0};
    uint32_t failure_count = 0;
    bool refresh_requested = true;
    time_t next_refresh_epoch = 0;

    (void)arg;

    response_buffer = calloc(1, WEATHER_DEVICE_RESPONSE_BYTES);
    if (response_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate weather response buffer");
        vTaskDelete(NULL);
        return;
    }

    response.buffer = response_buffer;
    response.buffer_size = WEATHER_DEVICE_RESPONSE_BYTES;
    device_cloud_session_init(&session, response_buffer, WEATHER_DEVICE_RESPONSE_BYTES);

    while (true) {
        device_cloud_config_t config = {0};
        uint32_t notify_bits = 0;
        bool timed_out = false;
        time_t now = 0;
        TickType_t wait_ticks = portMAX_DELAY;

        xEventGroupWaitBits(s_connected_event_group,
                            s_connected_bit,
                            pdFALSE,
                            pdTRUE,
                            portMAX_DELAY);

        if (device_cloud_service_get_generation() != s_last_cloud_generation) {
            s_last_cloud_generation = device_cloud_service_get_generation();
            refresh_requested = true;
        }

        if (weather_device_wait_for_time_or_refresh(&refresh_requested) == portMAX_DELAY) {
            next_refresh_epoch = 0;
            continue;
        }

        if (device_cloud_service_get_config(&config) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        now = time(NULL);
        if (refresh_requested || (next_refresh_epoch == 0) || (now >= next_refresh_epoch)) {
            if (weather_device_fetch_once(&session, &response, request_body, sizeof(request_body)) == ESP_OK) {
                failure_count = 0;
            } else {
                ++failure_count;
                if (!weather_device_has_successful_snapshot()) {
                    weather_device_publish_simple_state(WEATHER_DATA_STATE_ERROR);
                }
            }

            next_refresh_epoch = time(NULL) + (time_t)weather_device_next_retry_seconds(failure_count, &config);
            refresh_requested = false;
        }

        now = time(NULL);
        if (next_refresh_epoch <= now) {
            wait_ticks = 0;
        } else {
            uint32_t wait_seconds = (uint32_t)(next_refresh_epoch - now);
            wait_ticks = pdMS_TO_TICKS(wait_seconds * 1000U);
        }

        if (xTaskNotifyWait(0, UINT32_MAX, &notify_bits, wait_ticks) == pdFALSE) {
            timed_out = true;
        }

        if ((notify_bits & WEATHER_DEVICE_NOTIFY_REFRESH) != 0U) {
            refresh_requested = true;
        }
        if (device_cloud_service_get_generation() != s_last_cloud_generation) {
            s_last_cloud_generation = device_cloud_service_get_generation();
            refresh_requested = true;
        }
        if (timed_out) {
            refresh_requested = true;
        }
    }
}

esp_err_t weather_device_service_provider_start(EventGroupHandle_t connected_event_group,
                                                EventBits_t connected_bit)
{
    BaseType_t task_created = pdFAIL;

    if ((connected_event_group == NULL) || (connected_bit == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_task_handle != NULL) {
        weather_device_service_provider_request_refresh();
        return ESP_OK;
    }

    s_connected_event_group = connected_event_group;
    s_connected_bit = connected_bit;
    s_last_cloud_generation = device_cloud_service_get_generation();
    weather_device_publish_simple_state(WEATHER_DATA_STATE_LOADING);

    task_created = xTaskCreate(weather_device_provider_task,
                               "weather_device",
                               WEATHER_DEVICE_TASK_STACK_SIZE,
                               NULL,
                               WEATHER_DEVICE_TASK_PRIORITY,
                               &s_task_handle);
    if (task_created != pdPASS) {
        s_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    weather_device_service_provider_request_refresh();
    return ESP_OK;
}

esp_err_t weather_device_service_provider_get_snapshot(weather_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_snapshot_lock);
    if (s_has_snapshot) {
        *snapshot = s_latest_snapshot;
    } else {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->state = WEATHER_DATA_STATE_LOADING;
        snapshot->condition = WEATHER_CONDITION_UNKNOWN;
        snapshot->is_daytime = true;
    }
    taskEXIT_CRITICAL(&s_snapshot_lock);

    return ESP_OK;
}

void weather_device_service_provider_request_refresh(void)
{
    if (s_task_handle == NULL) {
        return;
    }

    xTaskNotify(s_task_handle, WEATHER_DEVICE_NOTIFY_REFRESH, eSetBits);
}
