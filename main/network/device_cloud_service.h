#ifndef DEVICE_CLOUD_SERVICE_H
#define DEVICE_CLOUD_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_client.h"

#define DEVICE_CLOUD_URL_SIZE 256
#define DEVICE_CLOUD_ID_SIZE 64
#define DEVICE_CLOUD_HEADER_NAME_SIZE 64
#define DEVICE_CLOUD_HEADER_VALUE_SIZE 160

typedef struct {
    char display_state_url[DEVICE_CLOUD_URL_SIZE];
    char pull_tasks_url[DEVICE_CLOUD_URL_SIZE];
    char report_status_url[DEVICE_CLOUD_URL_SIZE];
    char client_id[DEVICE_CLOUD_ID_SIZE];
    char device_id[DEVICE_CLOUD_ID_SIZE];
    char auth_header_name[DEVICE_CLOUD_HEADER_NAME_SIZE];
    char auth_header_value[DEVICE_CLOUD_HEADER_VALUE_SIZE];
    uint32_t preload_window_hours;
    uint32_t display_poll_seconds;
    uint32_t task_poll_seconds;
} device_cloud_config_t;

typedef struct {
    char *buffer;
    size_t buffer_size;
    size_t length;
    int status_code;
    bool truncated;
} device_cloud_http_response_t;

typedef struct {
    esp_http_client_handle_t client;
    device_cloud_http_response_t *active_response;
    char *response_buffer;
    size_t response_buffer_size;
    char url[DEVICE_CLOUD_URL_SIZE];
    char auth_header_name[DEVICE_CLOUD_HEADER_NAME_SIZE];
    char auth_header_value[DEVICE_CLOUD_HEADER_VALUE_SIZE];
    uint32_t config_generation;
    int last_status_code;
    esp_err_t last_error;
} device_cloud_session_t;

esp_err_t device_cloud_service_init(void);
esp_err_t device_cloud_service_get_config(device_cloud_config_t *out_config);
esp_err_t device_cloud_service_update_from_json(const char *json, size_t json_len, bool *changed);
uint32_t device_cloud_service_get_generation(void);
esp_err_t device_cloud_service_build_device_request_json(char *buffer, size_t buffer_size);
void device_cloud_session_init(device_cloud_session_t *session, char *response_buffer, size_t response_buffer_size);
void device_cloud_session_deinit(device_cloud_session_t *session);
esp_err_t device_cloud_session_post_json(device_cloud_session_t *session,
                                         const char *url,
                                         const char *json_body,
                                         device_cloud_http_response_t *response);
esp_err_t device_cloud_service_post_json(const char *url,
                                         const char *json_body,
                                         device_cloud_http_response_t *response);

#endif
