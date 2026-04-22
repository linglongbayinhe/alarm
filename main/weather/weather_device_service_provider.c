#include "weather_device_service_provider.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "time_service.h"

#define DEVICE_API_URL "https://fc-mp-569ac274-a245-482f-994d-e065e5e73b0b.next.bspapp.com/device-service/getDeviceDisplayState"
#define CLIENT_ID      "client_1776770443964_jmxlxc"
#define DEVICE_ID      "dev_demo_001"

#define WEATHER_DEVICE_POST_BODY "{\"clientId\":\"" CLIENT_ID "\",\"deviceId\":\"" DEVICE_ID "\"}"
#define WEATHER_DEVICE_POLL_INTERVAL_MS 30000
#define WEATHER_DEVICE_HTTP_TIMEOUT_MS  10000
#define WEATHER_DEVICE_RESPONSE_BYTES   4096
#define WEATHER_DEVICE_TASK_STACK_SIZE  6144
#define WEATHER_DEVICE_TASK_PRIORITY    4
#define WEATHER_DEVICE_TIME_WAIT_MS     1000
#define WEATHER_DEVICE_TIME_LOG_MS      5000

static const char *TAG = "WEATHER_DEVICE";

typedef struct {
    char *data;
    size_t capacity;
    size_t length;
    bool truncated;
} weather_device_http_response_t;

static portMUX_TYPE s_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static weather_snapshot_t s_latest_snapshot;
static bool s_has_snapshot;
static bool s_has_successful_snapshot;
static EventGroupHandle_t s_connected_event_group;
static EventBits_t s_connected_bit;
static TaskHandle_t s_task_handle;

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

static esp_err_t weather_device_http_event_handler(esp_http_client_event_t *event)
{
    weather_device_http_response_t *response = NULL;
    size_t remaining = 0;
    size_t copy_len = 0;

    if ((event == NULL) || (event->user_data == NULL)) {
        return ESP_OK;
    }

    response = (weather_device_http_response_t *)event->user_data;

    if ((event->event_id != HTTP_EVENT_ON_DATA) || (event->data == NULL) || (event->data_len <= 0)) {
        return ESP_OK;
    }

    if ((response->data == NULL) || (response->capacity == 0)) {
        return ESP_OK;
    }

    if ((response->length + 1) >= response->capacity) {
        response->truncated = true;
        return ESP_OK;
    }

    remaining = response->capacity - response->length - 1;
    copy_len = (size_t)event->data_len;
    if (copy_len > remaining) {
        copy_len = remaining;
        response->truncated = true;
    }

    memcpy(response->data + response->length, event->data, copy_len);
    response->length += copy_len;
    response->data[response->length] = '\0';

    return ESP_OK;
}

static esp_err_t weather_device_parse_response(const char *json, weather_snapshot_t *snapshot)
{
    esp_err_t ret = ESP_FAIL;
    cJSON *root = NULL;
    const cJSON *success = NULL;
    const cJSON *temperature = NULL;
    const cJSON *weather_icon = NULL;
    const cJSON *weather_text = NULL;
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

    ESP_LOGI(TAG,
             "Parsed device weather: temperature=%d, weatherIcon=%s, weatherText=%s",
             (int)snapshot->current_temperature_c,
             snapshot->weather_icon_text,
             snapshot->has_weather_text ? snapshot->weather_text : "");
    ret = ESP_OK;

cleanup:
    cJSON_Delete(root);
    return ret;
}

static esp_err_t weather_device_fetch_once(void)
{
    esp_err_t ret = ESP_OK;
    int status_code = 0;
    weather_device_http_response_t response = {0};
    weather_snapshot_t snapshot = {0};
    esp_http_client_handle_t client = NULL;
    esp_http_client_config_t config = {
        .url = DEVICE_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = WEATHER_DEVICE_HTTP_TIMEOUT_MS,
        .event_handler = weather_device_http_event_handler,
        .user_data = &response,
#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
        .skip_cert_common_name_check = true,
#else
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    response.data = (char *)calloc(1, WEATHER_DEVICE_RESPONSE_BYTES);
    if (response.data == NULL) {
        return ESP_ERR_NO_MEM;
    }
    response.capacity = WEATHER_DEVICE_RESPONSE_BYTES;

    client = esp_http_client_init(&config);
    if (client == NULL) {
        free(response.data);
        return ESP_FAIL;
    }

#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    ESP_LOGW(TAG, "TLS server certificate verification is disabled for quick integration");
#endif
    ESP_LOGI(TAG, "POST %s clientId=%s deviceId=%s", DEVICE_API_URL, CLIENT_ID, DEVICE_ID);
    ret = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Content-Type header: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ret = esp_http_client_set_header(client, "Accept", "application/json");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Accept header: %s", esp_err_to_name(ret));
        goto cleanup;
    }
    ret = esp_http_client_set_post_field(client,
                                         WEATHER_DEVICE_POST_BODY,
                                         (int)strlen(WEATHER_DEVICE_POST_BODY));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set POST body: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Device service request failed: %s", esp_err_to_name(ret));
        goto cleanup;
    }

    status_code = esp_http_client_get_status_code(client);
    if (response.truncated) {
        ESP_LOGW(TAG, "Device service response truncated to %u bytes",
                 (unsigned int)(WEATHER_DEVICE_RESPONSE_BYTES - 1));
    }
    ESP_LOGI(TAG, "Raw JSON: %s", response.data);

    if ((status_code < 200) || (status_code >= 300)) {
        ESP_LOGE(TAG, "Device service returned HTTP status %d", status_code);
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = weather_device_parse_response(response.data, &snapshot);
    if (ret == ESP_OK) {
        weather_device_publish_snapshot(&snapshot, true);
    }

cleanup:
    esp_http_client_cleanup(client);
    free(response.data);
    return ret;
}

static bool weather_device_wait_for_valid_time(void)
{
    int waited_ms = 0;

    while (!time_service_has_valid_time()) {
        EventBits_t bits = xEventGroupGetBits(s_connected_event_group);

        if ((bits & s_connected_bit) == 0) {
            ESP_LOGW(TAG, "Wi-Fi disconnected while waiting for valid time");
            return false;
        }

        if ((waited_ms % WEATHER_DEVICE_TIME_LOG_MS) == 0) {
            ESP_LOGI(TAG, "Waiting for valid system time before HTTPS request");
        }

        weather_device_publish_simple_state(WEATHER_DATA_STATE_LOADING);
        vTaskDelay(pdMS_TO_TICKS(WEATHER_DEVICE_TIME_WAIT_MS));
        waited_ms += WEATHER_DEVICE_TIME_WAIT_MS;
    }

    if (waited_ms > 0) {
        ESP_LOGI(TAG, "System time is valid; starting weather HTTPS request");
    }

    return true;
}

static void weather_device_provider_task(void *arg)
{
    (void)arg;

    while (true) {
        xEventGroupWaitBits(s_connected_event_group,
                            s_connected_bit,
                            pdFALSE,
                            pdTRUE,
                            portMAX_DELAY);

        if (!weather_device_wait_for_valid_time()) {
            continue;
        }

        if (weather_device_fetch_once() != ESP_OK) {
            if (!weather_device_has_successful_snapshot()) {
                weather_device_publish_simple_state(WEATHER_DATA_STATE_ERROR);
            }
        }

        for (int elapsed_ms = 0;
             elapsed_ms < WEATHER_DEVICE_POLL_INTERVAL_MS;
             elapsed_ms += 1000) {
            EventBits_t bits = xEventGroupGetBits(s_connected_event_group);
            if ((bits & s_connected_bit) == 0) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
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
        return ESP_OK;
    }

    s_connected_event_group = connected_event_group;
    s_connected_bit = connected_bit;
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
