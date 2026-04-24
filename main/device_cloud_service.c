#include "device_cloud_service.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"

static const char *TAG = "DEVICE_CLOUD";
static const char *DEVICE_CLOUD_NAMESPACE = "dev_cloud";
static const char *DEVICE_CLOUD_CONFIG_KEY = "config_v1";

#define DEVICE_CLOUD_DEFAULT_DISPLAY_URL "https://fc-mp-569ac274-a245-482f-994d-e065e5e73b0b.next.bspapp.com/device-service/getDeviceDisplayState"
#define DEVICE_CLOUD_DEFAULT_PULL_URL    "https://fc-mp-569ac274-a245-482f-994d-e065e5e73b0b.next.bspapp.com/device-service/pullPlaybackTasks"
#define DEVICE_CLOUD_DEFAULT_REPORT_URL  "https://fc-mp-569ac274-a245-482f-994d-e065e5e73b0b.next.bspapp.com/device-service/reportPlaybackTaskStatus"
#define DEVICE_CLOUD_DEFAULT_CLIENT_ID   "client_1776770443964_jmxlxc"
#define DEVICE_CLOUD_DEFAULT_DEVICE_ID   "dev_demo_001"
#define DEVICE_CLOUD_DEFAULT_PRELOAD_HOURS 48
#define DEVICE_CLOUD_DEFAULT_DISPLAY_POLL_SECONDS (15U * 60U)
#define DEVICE_CLOUD_DEFAULT_TASK_POLL_SECONDS (5U * 60U)
#define DEVICE_CLOUD_HTTP_TIMEOUT_MS 10000

static portMUX_TYPE s_config_lock = portMUX_INITIALIZER_UNLOCKED;
static device_cloud_config_t s_config;
static bool s_initialized;
static uint32_t s_generation;

static void device_cloud_copy_string(char *destination, size_t destination_size, const char *source)
{
    if ((destination == NULL) || (destination_size == 0)) {
        return;
    }

    snprintf(destination, destination_size, "%s", source == NULL ? "" : source);
}

static bool device_cloud_string_is_empty(const char *value)
{
    return (value == NULL) || (value[0] == '\0');
}

static bool device_cloud_strings_equal(const char *left, const char *right)
{
    if ((left == NULL) || (right == NULL)) {
        return false;
    }

    return strcmp(left, right) == 0;
}

static void device_cloud_init_defaults(device_cloud_config_t *config)
{
    memset(config, 0, sizeof(*config));
    device_cloud_copy_string(config->display_state_url,
                             sizeof(config->display_state_url),
                             DEVICE_CLOUD_DEFAULT_DISPLAY_URL);
    device_cloud_copy_string(config->pull_tasks_url,
                             sizeof(config->pull_tasks_url),
                             DEVICE_CLOUD_DEFAULT_PULL_URL);
    device_cloud_copy_string(config->report_status_url,
                             sizeof(config->report_status_url),
                             DEVICE_CLOUD_DEFAULT_REPORT_URL);
    device_cloud_copy_string(config->client_id, sizeof(config->client_id), DEVICE_CLOUD_DEFAULT_CLIENT_ID);
    device_cloud_copy_string(config->device_id, sizeof(config->device_id), DEVICE_CLOUD_DEFAULT_DEVICE_ID);
    config->preload_window_hours = DEVICE_CLOUD_DEFAULT_PRELOAD_HOURS;
    config->display_poll_seconds = DEVICE_CLOUD_DEFAULT_DISPLAY_POLL_SECONDS;
    config->task_poll_seconds = DEVICE_CLOUD_DEFAULT_TASK_POLL_SECONDS;
}

static void device_cloud_derive_url(const char *display_url,
                                    const char *endpoint_name,
                                    char *destination,
                                    size_t destination_size)
{
    const char *last_slash = NULL;
    size_t prefix_length = 0;

    if ((display_url == NULL) || (endpoint_name == NULL) ||
        (destination == NULL) || (destination_size == 0)) {
        return;
    }

    last_slash = strrchr(display_url, '/');
    if (last_slash == NULL) {
        return;
    }

    prefix_length = (size_t)(last_slash - display_url);
    if ((prefix_length + 1 + strlen(endpoint_name) + 1) > destination_size) {
        return;
    }

    memcpy(destination, display_url, prefix_length);
    destination[prefix_length] = '/';
    destination[prefix_length + 1] = '\0';
    strncat(destination, endpoint_name, destination_size - strlen(destination) - 1);
}

static void device_cloud_apply_defaults(device_cloud_config_t *config)
{
    if (config == NULL) {
        return;
    }

    if (device_cloud_string_is_empty(config->display_state_url)) {
        device_cloud_copy_string(config->display_state_url,
                                 sizeof(config->display_state_url),
                                 DEVICE_CLOUD_DEFAULT_DISPLAY_URL);
    }
    if (device_cloud_string_is_empty(config->pull_tasks_url)) {
        device_cloud_derive_url(config->display_state_url,
                                "pullPlaybackTasks",
                                config->pull_tasks_url,
                                sizeof(config->pull_tasks_url));
    }
    if (device_cloud_string_is_empty(config->report_status_url)) {
        device_cloud_derive_url(config->display_state_url,
                                "reportPlaybackTaskStatus",
                                config->report_status_url,
                                sizeof(config->report_status_url));
    }
    if (device_cloud_string_is_empty(config->pull_tasks_url)) {
        device_cloud_copy_string(config->pull_tasks_url,
                                 sizeof(config->pull_tasks_url),
                                 DEVICE_CLOUD_DEFAULT_PULL_URL);
    }
    if (device_cloud_string_is_empty(config->report_status_url)) {
        device_cloud_copy_string(config->report_status_url,
                                 sizeof(config->report_status_url),
                                 DEVICE_CLOUD_DEFAULT_REPORT_URL);
    }
    if (device_cloud_string_is_empty(config->client_id)) {
        device_cloud_copy_string(config->client_id, sizeof(config->client_id), DEVICE_CLOUD_DEFAULT_CLIENT_ID);
    }
    if (device_cloud_string_is_empty(config->device_id)) {
        device_cloud_copy_string(config->device_id, sizeof(config->device_id), DEVICE_CLOUD_DEFAULT_DEVICE_ID);
    }
    if (config->preload_window_hours == 0) {
        config->preload_window_hours = DEVICE_CLOUD_DEFAULT_PRELOAD_HOURS;
    }
    if ((config->display_poll_seconds == 0U) || (config->display_poll_seconds == 30U)) {
        config->display_poll_seconds = DEVICE_CLOUD_DEFAULT_DISPLAY_POLL_SECONDS;
    }
    if ((config->task_poll_seconds == 0U) || (config->task_poll_seconds == 30U)) {
        config->task_poll_seconds = DEVICE_CLOUD_DEFAULT_TASK_POLL_SECONDS;
    }
}

static esp_err_t device_cloud_save_config(const device_cloud_config_t *config)
{
    nvs_handle_t handle = 0;
    esp_err_t ret = ESP_OK;

    ret = nvs_open(DEVICE_CLOUD_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_blob(handle, DEVICE_CLOUD_CONFIG_KEY, config, sizeof(*config));
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }

    nvs_close(handle);
    return ret;
}

static void device_cloud_store_config(const device_cloud_config_t *config)
{
    taskENTER_CRITICAL(&s_config_lock);
    s_config = *config;
    ++s_generation;
    taskEXIT_CRITICAL(&s_config_lock);
}

static void device_cloud_try_set_string(const cJSON *root,
                                        const char *primary_key,
                                        const char *secondary_key,
                                        char *destination,
                                        size_t destination_size)
{
    const cJSON *item = NULL;

    if ((root == NULL) || (destination == NULL) || (destination_size == 0)) {
        return;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, primary_key);
    if ((item == NULL) && (secondary_key != NULL)) {
        item = cJSON_GetObjectItemCaseSensitive(root, secondary_key);
    }
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        device_cloud_copy_string(destination, destination_size, item->valuestring);
    }
}

static void device_cloud_try_set_u32(const cJSON *root,
                                     const char *primary_key,
                                     const char *secondary_key,
                                     uint32_t *destination)
{
    const cJSON *item = NULL;

    if ((root == NULL) || (destination == NULL)) {
        return;
    }

    item = cJSON_GetObjectItemCaseSensitive(root, primary_key);
    if ((item == NULL) && (secondary_key != NULL)) {
        item = cJSON_GetObjectItemCaseSensitive(root, secondary_key);
    }
    if (cJSON_IsNumber(item) && (item->valuedouble >= 0.0)) {
        *destination = (uint32_t)item->valuedouble;
    }
}

static esp_err_t device_cloud_http_event_handler(esp_http_client_event_t *event)
{
    device_cloud_session_t *session = NULL;
    device_cloud_http_response_t *response = NULL;
    size_t remaining = 0;
    size_t copy_length = 0;

    if ((event == NULL) || (event->user_data == NULL)) {
        return ESP_OK;
    }

    session = (device_cloud_session_t *)event->user_data;
    response = session->active_response;
    if ((event->event_id != HTTP_EVENT_ON_DATA) || (event->data == NULL) || (event->data_len <= 0)) {
        return ESP_OK;
    }
    if ((response->buffer == NULL) || (response->buffer_size == 0)) {
        return ESP_OK;
    }
    if ((response->length + 1) >= response->buffer_size) {
        response->truncated = true;
        return ESP_OK;
    }

    remaining = response->buffer_size - response->length - 1;
    copy_length = (size_t)event->data_len;
    if (copy_length > remaining) {
        copy_length = remaining;
        response->truncated = true;
    }

    memcpy(response->buffer + response->length, event->data, copy_length);
    response->length += copy_length;
    response->buffer[response->length] = '\0';

    return ESP_OK;
}

static void device_cloud_session_reset_client(device_cloud_session_t *session)
{
    if ((session == NULL) || (session->client == NULL)) {
        return;
    }

    esp_http_client_cleanup(session->client);
    session->client = NULL;
    session->url[0] = '\0';
    session->auth_header_name[0] = '\0';
    session->auth_header_value[0] = '\0';
    session->config_generation = 0;
}

static bool device_cloud_session_needs_rebuild(const device_cloud_session_t *session,
                                               const char *url,
                                               const device_cloud_config_t *config_snapshot,
                                               uint32_t generation)
{
    if ((session == NULL) || (url == NULL) || (config_snapshot == NULL)) {
        return true;
    }
    if (session->client == NULL) {
        return true;
    }
    if (session->config_generation != generation) {
        return true;
    }
    if (!device_cloud_strings_equal(session->url, url)) {
        return true;
    }
    if (!device_cloud_strings_equal(session->auth_header_name, config_snapshot->auth_header_name)) {
        return true;
    }
    if (!device_cloud_strings_equal(session->auth_header_value, config_snapshot->auth_header_value)) {
        return true;
    }

    return false;
}

static esp_err_t device_cloud_session_prepare_client(device_cloud_session_t *session,
                                                     const char *url,
                                                     const device_cloud_config_t *config_snapshot,
                                                     uint32_t generation)
{
    esp_http_client_config_t client_config = {0};

    if ((session == NULL) || (url == NULL) || (config_snapshot == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!device_cloud_session_needs_rebuild(session, url, config_snapshot, generation)) {
        ESP_LOGI(TAG, "Reusing HTTP session for %s", url);
        return ESP_OK;
    }

    if (session->client != NULL) {
        ESP_LOGI(TAG, "Rebuilding HTTP session for %s", url);
    } else {
        ESP_LOGI(TAG, "Creating HTTP session for %s", url);
    }
    device_cloud_session_reset_client(session);

    client_config.url = url;
    client_config.method = HTTP_METHOD_POST;
    client_config.timeout_ms = DEVICE_CLOUD_HTTP_TIMEOUT_MS;
    client_config.event_handler = device_cloud_http_event_handler;
    client_config.user_data = session;
    client_config.keep_alive_enable = true;
#if defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
    client_config.skip_cert_common_name_check = true;
#else
    client_config.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    session->client = esp_http_client_init(&client_config);
    if (session->client == NULL) {
        return ESP_FAIL;
    }

    device_cloud_copy_string(session->url, sizeof(session->url), url);
    device_cloud_copy_string(session->auth_header_name,
                             sizeof(session->auth_header_name),
                             config_snapshot->auth_header_name);
    device_cloud_copy_string(session->auth_header_value,
                             sizeof(session->auth_header_value),
                             config_snapshot->auth_header_value);
    session->config_generation = generation;

    return ESP_OK;
}

esp_err_t device_cloud_service_init(void)
{
    device_cloud_config_t loaded_config;
    nvs_handle_t handle = 0;
    size_t required_size = sizeof(loaded_config);
    esp_err_t ret = ESP_OK;

    if (s_initialized) {
        return ESP_OK;
    }

    device_cloud_init_defaults(&loaded_config);

    ret = nvs_open(DEVICE_CLOUD_NAMESPACE, NVS_READWRITE, &handle);
    if (ret == ESP_OK) {
        ret = nvs_get_blob(handle, DEVICE_CLOUD_CONFIG_KEY, &loaded_config, &required_size);
        if ((ret != ESP_OK) || (required_size != sizeof(loaded_config))) {
            device_cloud_init_defaults(&loaded_config);
            ret = device_cloud_save_config(&loaded_config);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to persist default config: %s", esp_err_to_name(ret));
            }
        }
        nvs_close(handle);
    } else {
        ESP_LOGW(TAG, "Failed to open NVS for cloud config: %s", esp_err_to_name(ret));
    }

    device_cloud_apply_defaults(&loaded_config);
    device_cloud_store_config(&loaded_config);
    s_initialized = true;

    ESP_LOGI(TAG, "Cloud config initialized for deviceId=%s", loaded_config.device_id);
    return ESP_OK;
}

esp_err_t device_cloud_service_get_config(device_cloud_config_t *out_config)
{
    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    taskENTER_CRITICAL(&s_config_lock);
    *out_config = s_config;
    taskEXIT_CRITICAL(&s_config_lock);

    return ESP_OK;
}

esp_err_t device_cloud_service_update_from_json(const char *json, size_t json_len, bool *changed)
{
    device_cloud_config_t current_config;
    device_cloud_config_t updated_config;
    cJSON *root = NULL;
    cJSON *config_object = NULL;
    esp_err_t ret = ESP_OK;

    if ((json == NULL) || (json_len == 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (changed != NULL) {
        *changed = false;
    }

    ret = device_cloud_service_get_config(&current_config);
    if (ret != ESP_OK) {
        return ret;
    }

    updated_config = current_config;
    root = cJSON_ParseWithLength(json, json_len);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse custom cloud config JSON");
        return ESP_ERR_INVALID_ARG;
    }

    config_object = cJSON_GetObjectItemCaseSensitive(root, "cloudConfig");
    if (!cJSON_IsObject(config_object)) {
        config_object = root;
    }

    device_cloud_try_set_string(config_object,
                                "display_state_url",
                                "displayStateUrl",
                                updated_config.display_state_url,
                                sizeof(updated_config.display_state_url));
    device_cloud_try_set_string(config_object,
                                "pull_tasks_url",
                                "pullTasksUrl",
                                updated_config.pull_tasks_url,
                                sizeof(updated_config.pull_tasks_url));
    device_cloud_try_set_string(config_object,
                                "report_status_url",
                                "reportStatusUrl",
                                updated_config.report_status_url,
                                sizeof(updated_config.report_status_url));
    device_cloud_try_set_string(config_object,
                                "client_id",
                                "clientId",
                                updated_config.client_id,
                                sizeof(updated_config.client_id));
    device_cloud_try_set_string(config_object,
                                "device_id",
                                "deviceId",
                                updated_config.device_id,
                                sizeof(updated_config.device_id));
    device_cloud_try_set_string(config_object,
                                "auth_header_name",
                                "authHeaderName",
                                updated_config.auth_header_name,
                                sizeof(updated_config.auth_header_name));
    device_cloud_try_set_string(config_object,
                                "auth_header_value",
                                "authHeaderValue",
                                updated_config.auth_header_value,
                                sizeof(updated_config.auth_header_value));
    device_cloud_try_set_u32(config_object,
                             "preload_window_hours",
                             "preloadWindowHours",
                             &updated_config.preload_window_hours);
    device_cloud_try_set_u32(config_object,
                             "display_poll_seconds",
                             "displayPollSeconds",
                             &updated_config.display_poll_seconds);
    device_cloud_try_set_u32(config_object,
                             "task_poll_seconds",
                             "taskPollSeconds",
                             &updated_config.task_poll_seconds);

    device_cloud_apply_defaults(&updated_config);
    if (memcmp(&current_config, &updated_config, sizeof(updated_config)) == 0) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    ret = device_cloud_save_config(&updated_config);
    if (ret != ESP_OK) {
        cJSON_Delete(root);
        return ret;
    }

    device_cloud_store_config(&updated_config);
    if (changed != NULL) {
        *changed = true;
    }

    ESP_LOGI(TAG, "Cloud config updated via BLUFI custom data");
    cJSON_Delete(root);
    return ESP_OK;
}

uint32_t device_cloud_service_get_generation(void)
{
    uint32_t generation = 0;

    taskENTER_CRITICAL(&s_config_lock);
    generation = s_generation;
    taskEXIT_CRITICAL(&s_config_lock);

    return generation;
}

esp_err_t device_cloud_service_build_device_request_json(char *buffer, size_t buffer_size)
{
    device_cloud_config_t config;
    int written = 0;
    esp_err_t ret = ESP_OK;

    if ((buffer == NULL) || (buffer_size == 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = device_cloud_service_get_config(&config);
    if (ret != ESP_OK) {
        return ret;
    }

    written = snprintf(buffer,
                       buffer_size,
                       "{\"clientId\":\"%s\",\"deviceId\":\"%s\"}",
                       config.client_id,
                       config.device_id);
    if ((written < 0) || ((size_t)written >= buffer_size)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

void device_cloud_session_init(device_cloud_session_t *session, char *response_buffer, size_t response_buffer_size)
{
    if (session == NULL) {
        return;
    }

    memset(session, 0, sizeof(*session));
    session->response_buffer = response_buffer;
    session->response_buffer_size = response_buffer_size;
}

void device_cloud_session_deinit(device_cloud_session_t *session)
{
    if (session == NULL) {
        return;
    }

    device_cloud_session_reset_client(session);
    session->active_response = NULL;
    session->last_status_code = 0;
    session->last_error = ESP_OK;
}

esp_err_t device_cloud_session_post_json(device_cloud_session_t *session,
                                         const char *url,
                                         const char *json_body,
                                         device_cloud_http_response_t *response)
{
    device_cloud_config_t config_snapshot;
    device_cloud_http_response_t internal_response = {0};
    esp_err_t ret = ESP_OK;
    uint32_t generation = 0;

    if ((session == NULL) || (url == NULL) || (url[0] == '\0') || (json_body == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = device_cloud_service_get_config(&config_snapshot);
    if (ret != ESP_OK) {
        return ret;
    }
    generation = device_cloud_service_get_generation();

    ret = device_cloud_session_prepare_client(session, url, &config_snapshot, generation);
    if (ret != ESP_OK) {
        session->last_error = ret;
        return ret;
    }

    if (response == NULL) {
        internal_response.buffer = session->response_buffer;
        internal_response.buffer_size = session->response_buffer_size;
        response = &internal_response;
    }

    if (response != NULL) {
        response->length = 0;
        response->status_code = 0;
        response->truncated = false;
        if ((response->buffer != NULL) && (response->buffer_size > 0)) {
            response->buffer[0] = '\0';
        }
    }
    session->active_response = response;

    ret = esp_http_client_set_header(session->client, "Content-Type", "application/json");
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(session->client, "Accept", "application/json");
    }
    if ((ret == ESP_OK) &&
        !device_cloud_string_is_empty(config_snapshot.auth_header_name) &&
        !device_cloud_string_is_empty(config_snapshot.auth_header_value)) {
        ret = esp_http_client_set_header(session->client,
                                         config_snapshot.auth_header_name,
                                         config_snapshot.auth_header_value);
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_set_post_field(session->client, json_body, (int)strlen(json_body));
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_perform(session->client);
    }
    if (ret == ESP_OK) {
        int status_code = esp_http_client_get_status_code(session->client);
        if (response != NULL) {
            response->status_code = status_code;
        }
        session->last_status_code = status_code;
        if ((status_code < 200) || (status_code >= 300)) {
            ret = ESP_FAIL;
        }
    } else {
        device_cloud_session_reset_client(session);
    }

    session->last_error = ret;
    session->active_response = NULL;
    return ret;
}

esp_err_t device_cloud_service_post_json(const char *url,
                                         const char *json_body,
                                         device_cloud_http_response_t *response)
{
    device_cloud_session_t session = {0};
    esp_err_t ret = ESP_OK;

    device_cloud_session_init(&session,
                              response == NULL ? NULL : response->buffer,
                              response == NULL ? 0 : response->buffer_size);
    ret = device_cloud_session_post_json(&session, url, json_body, response);
    device_cloud_session_deinit(&session);
    return ret;
}
