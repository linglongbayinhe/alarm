/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */


#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_lifecycle.h"
#include "blufi_service.h"
#include "blufi_port.h"

#include "audio_cache_service.h"
#include "audio_service.h"
#include "button_service.h"
#include "device_cloud_service.h"
#include "device_info.h"
#include "device_utils.h"
#include "display_service.h"
#include "network_task_service.h"
#include "playback_task_service.h"
#include "rtc_service.h"
#include "status_presenter.h"
#include "storage_service.h"
#include "time_service.h"
#include "weather_service.h"

#ifndef CONFIG_SOC_BLUFI_SUPPORTED
#error "This SOC does not support BLUFI"
#endif

#define EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY CONFIG_EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY
#define EXAMPLE_INVALID_REASON                255
#define EXAMPLE_INVALID_RSSI                  -128
#define EXAMPLE_PROVISIONING_SUCCESS_VISIBLE_MS 1500
#define EXAMPLE_PROVISIONING_QR_FIELD_SIZE    48

static void app_ui_task(void *arg);
static esp_err_t app_start_display_services(const char *reason);
static void app_schedule_runtime_transition(void);
static void app_startup_pull_done(esp_err_t ret, void *ctx);
static void app_startup_weather_done(esp_err_t ret, void *ctx);
static bool app_blufi_get_wifi_status(void *ctx, blufi_service_wifi_status_t *out_status);
static void app_blufi_schedule_runtime_transition(void *ctx);
static void app_blufi_request_wifi_connect(void *ctx);
static void app_blufi_request_wifi_disconnect(void *ctx);
static void app_blufi_request_startup_playback_pull(void *ctx);
static void app_blufi_request_startup_weather_refresh(void *ctx);
static void app_blufi_request_weather_refresh(void *ctx);
static void app_blufi_request_playback_sync(void *ctx);
static void app_blufi_cloud_config_changed(void *ctx);

#define EXAMPLE_UI_TASK_STACK_SIZE 3072
#define EXAMPLE_UI_TASK_PRIORITY   5
#define EXAMPLE_RUNTIME_TRANSITION_TASK_STACK_SIZE 4096
#define EXAMPLE_RUNTIME_TRANSITION_TASK_PRIORITY   4

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static uint8_t s_wifi_retry = 0;

typedef struct {
    bool connected;
    bool is_got_ip;
    bool connecting;
    bool has_stored_config;
    uint8_t bssid[6];
    uint8_t ssid[32];
    int ssid_len;
} app_wifi_state_t;

typedef struct {
    bool network_started;
    bool services_started;
    bool startup_pull_requested;
    bool startup_pull_done;
    bool startup_weather_requested;
    bool startup_weather_done;
    bool network_degraded;
    bool weather_degraded;
    TaskHandle_t transition_task;
} app_runtime_state_t;

typedef struct {
    display_provisioning_state_t state;
    TickType_t success_hide_at;
    char qr_payload[DISPLAY_PROVISIONING_QR_PAYLOAD_SIZE];
} app_provisioning_ui_state_t;

static app_wifi_state_t s_wifi_state;
static app_runtime_state_t s_runtime_state;
static app_provisioning_ui_state_t s_provisioning_ui_state;
static bool s_display_services_started = false;

static void app_log_internal_heap(const char *label)
{
    BLUFI_INFO("Heap %s: free=%u largest=%u\n",
               label,
               (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static const char *app_wifi_reason_name(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL:
            return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL:
            return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL:
            return "CONNECTION_FAIL";
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
            return "NO_AP_COMPATIBLE_SECURITY";
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
            return "NO_AP_AUTHMODE_THRESHOLD";
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return "NO_AP_RSSI_THRESHOLD";
        case WIFI_REASON_BEACON_TIMEOUT:
            return "BEACON_TIMEOUT";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return "4WAY_HANDSHAKE_TIMEOUT";
        default:
            return "OTHER";
    }
}

/* Restores the system clock from RTC before network time becomes available. */
static void app_sync_time_from_rtc(void)
{
    struct tm rtc_time = {0};
    esp_err_t ret = ESP_OK;

    if (!rtc_service_is_ready()) {
        return;
    }
    if (!rtc_service_has_valid_time()) {
        BLUFI_INFO("RTC time is not valid yet\n");
        return;
    }

    ret = rtc_service_read(&rtc_time);
    if (ret != ESP_OK) {
        BLUFI_ERROR("RTC read failed: %s\n", esp_err_to_name(ret));
        return;
    }

    ret = time_service_set_local_time(&rtc_time);
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to restore system time from RTC: %s\n", esp_err_to_name(ret));
        return;
    }

    BLUFI_INFO("System time restored from RTC\n");
}

/* Writes synchronized system time back to RTC after SNTP succeeds. */
static void app_sync_rtc_from_system_time(void)
{
    struct tm local_time = {0};
    esp_err_t ret = ESP_OK;

    if (!rtc_service_is_ready() || !time_service_has_valid_time()) {
        return;
    }

    ret = time_service_get_local_time(&local_time);
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to read local time for RTC sync: %s\n", esp_err_to_name(ret));
        return;
    }

    ret = rtc_service_write(&local_time);
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to write system time to RTC: %s\n", esp_err_to_name(ret));
        return;
    }

    BLUFI_INFO("RTC updated from synchronized system time\n");
}

/* Reads the connected AP RSSI and reports whether the value is available. */
static bool app_get_wifi_rssi(int *rssi_out)
{
    wifi_ap_record_t ap_record = {0};
    esp_err_t ret = ESP_OK;

    if ((rssi_out == NULL) || !s_wifi_state.is_got_ip) {
        return false;
    }

    ret = esp_wifi_sta_get_ap_info(&ap_record);
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to read Wi-Fi RSSI: %s\n", esp_err_to_name(ret));
        return false;
    }

    *rssi_out = ap_record.rssi;

    return true;
}

static void app_provisioning_set_state(display_provisioning_state_t state)
{
    s_provisioning_ui_state.state = state;
    if (state == DISPLAY_PROVISIONING_STATE_SUCCESS) {
        s_provisioning_ui_state.success_hide_at =
            xTaskGetTickCount() + pdMS_TO_TICKS(EXAMPLE_PROVISIONING_SUCCESS_VISIBLE_MS);
    } else {
        s_provisioning_ui_state.success_hide_at = 0;
    }
}

static void app_provisioning_update_success_timeout(void)
{
    if (s_provisioning_ui_state.state != DISPLAY_PROVISIONING_STATE_SUCCESS) {
        return;
    }
    if ((int32_t)(xTaskGetTickCount() - s_provisioning_ui_state.success_hide_at) >= 0) {
        app_provisioning_set_state(DISPLAY_PROVISIONING_STATE_HIDDEN);
    }
}

static void app_json_escape_copy(char *destination, size_t destination_size, const char *source)
{
    size_t used = 0;

    if ((destination == NULL) || (destination_size == 0)) {
        return;
    }

    if (source == NULL) {
        source = "";
    }

    while ((*source != '\0') && (used + 1 < destination_size)) {
        if ((*source == '\\') || (*source == '"')) {
            if (used + 2 >= destination_size) {
                break;
            }
            destination[used++] = '\\';
            destination[used++] = *source++;
            continue;
        }
        destination[used++] = *source++;
    }
    destination[used] = '\0';
}

static void app_prepare_provisioning_qr_payload(void)
{
    char escaped_device_id[EXAMPLE_PROVISIONING_QR_FIELD_SIZE];
    char escaped_ble_name[EXAMPLE_PROVISIONING_QR_FIELD_SIZE];
    char device_id[DEVICE_ID_SIZE] = {0};
    int written;
    esp_err_t ret;

    ret = device_utils_get_device_id(device_id, sizeof(device_id));
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to build device id for QR payload: %s\n", esp_err_to_name(ret));
        snprintf(device_id, sizeof(device_id), "%s", "unknown");
    }

    app_json_escape_copy(escaped_device_id, sizeof(escaped_device_id), device_id);
    app_json_escape_copy(escaped_ble_name, sizeof(escaped_ble_name), CUSTOM_BLUFI_DEVICE_NAME);
    written = snprintf(s_provisioning_ui_state.qr_payload,
                       sizeof(s_provisioning_ui_state.qr_payload),
                       "{\"type\":\"alarm_ble\",\"ver\":1,\"deviceId\":\"%s\",\"bleName\":\"%s\"}",
                       escaped_device_id,
                       escaped_ble_name);
    if ((written < 0) || ((size_t)written >= sizeof(s_provisioning_ui_state.qr_payload))) {
        BLUFI_ERROR("Provisioning QR payload was truncated\n");
        s_provisioning_ui_state.qr_payload[0] = '\0';
    }
}

static void app_apply_provisioning_view(display_view_model_t *view_model)
{
    if (view_model == NULL) {
        return;
    }

    app_provisioning_update_success_timeout();
    view_model->provisioning_state = s_provisioning_ui_state.state;
    snprintf(view_model->provisioning_qr_payload,
             sizeof(view_model->provisioning_qr_payload),
             "%s",
             s_provisioning_ui_state.qr_payload);
}

static esp_err_t app_start_display_services(const char *reason)
{
    BaseType_t task_created;
    esp_err_t ret = ESP_OK;

    if (s_display_services_started) {
        return ESP_OK;
    }

    BLUFI_INFO("Starting display services: %s\n", reason == NULL ? "unknown" : reason);
    app_log_internal_heap("before_display_service_init");
    ret = display_service_init();
    if (ret != ESP_OK) {
        BLUFI_ERROR("Display service init failed: %s\n", esp_err_to_name(ret));
        return ret;
    }
    app_log_internal_heap("after_display_service_init");

    task_created = xTaskCreate(app_ui_task,
                               "app_ui_task",
                               EXAMPLE_UI_TASK_STACK_SIZE,
                               NULL,
                               EXAMPLE_UI_TASK_PRIORITY,
                               NULL);
    if (task_created != pdPASS) {
        BLUFI_ERROR("Failed to create UI task\n");
        return ESP_ERR_NO_MEM;
    }

    s_display_services_started = true;
    app_log_internal_heap("after_ui_task_start");
    return ESP_OK;
}

static esp_err_t app_start_runtime_services(void)
{
    esp_err_t ret = ESP_OK;

    if (s_runtime_state.services_started) {
        weather_service_request_refresh();
        playback_task_service_request_sync();
        return ESP_OK;
    }
    if (!s_wifi_state.is_got_ip) {
        return ESP_ERR_INVALID_STATE;
    }
    if (blufi_service_is_runtime_blocked()) {
        BLUFI_INFO("Deferring runtime service start until BLUFI memory is released\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_runtime_state.network_started) {
        app_log_internal_heap("before_network_start");
        ret = network_task_service_start(wifi_event_group, CONNECTED_BIT);
        if (ret != ESP_OK) {
            return ret;
        }
        s_runtime_state.network_started = true;
        app_log_internal_heap("after_network_start");
    }

    if (!s_runtime_state.startup_pull_done) {
        if (!s_runtime_state.startup_pull_requested) {
            s_runtime_state.startup_pull_requested = true;
            network_task_service_request_playback_pull(NETWORK_TASK_PLAYBACK_REASON_STARTUP);
        }
        BLUFI_INFO("Deferring playback/weather start until startup pull completes\n");
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_runtime_state.startup_weather_done) {
        if (!s_runtime_state.startup_weather_requested) {
            s_runtime_state.startup_weather_requested = true;
            app_log_internal_heap("before_startup_weather_request");
            network_task_service_request_weather_refresh(NETWORK_TASK_WEATHER_REASON_STARTUP);
        }
        BLUFI_INFO("Deferring playback/weather task start until startup weather completes\n");
        return ESP_ERR_INVALID_STATE;
    }

    app_log_internal_heap("before_playback_start");
    ret = playback_task_service_start(wifi_event_group, CONNECTED_BIT);
    if (ret != ESP_OK) {
        return ret;
    }
    app_log_internal_heap("after_playback_start");

    app_log_internal_heap("before_weather_start");
    ret = weather_service_start(wifi_event_group, CONNECTED_BIT);
    if (ret != ESP_OK) {
        return ret;
    }
    app_log_internal_heap("after_weather_start");

    s_runtime_state.services_started = true;
    return ESP_OK;
}

static void app_startup_pull_done(esp_err_t ret, void *ctx)
{
    (void)ctx;

    s_runtime_state.startup_pull_done = true;
    s_runtime_state.network_degraded = (ret != ESP_OK);
    BLUFI_INFO("Startup pull completed: %s degraded=%d\n",
               esp_err_to_name(ret),
               s_runtime_state.network_degraded ? 1 : 0);
    app_schedule_runtime_transition();
}

static void app_startup_weather_done(esp_err_t ret, void *ctx)
{
    (void)ctx;

    s_runtime_state.startup_weather_done = true;
    s_runtime_state.weather_degraded = (ret != ESP_OK);
    BLUFI_INFO("Startup weather completed: %s degraded=%d\n",
               esp_err_to_name(ret),
               s_runtime_state.weather_degraded ? 1 : 0);
    app_log_internal_heap("after_startup_weather");
    app_schedule_runtime_transition();
}

static void app_runtime_transition_task(void *arg)
{
    esp_err_t ret = ESP_OK;

    (void)arg;

    if (s_wifi_state.is_got_ip) {
        ret = blufi_service_release_if_ready();
        if (ret != ESP_OK) {
            BLUFI_ERROR("Deferred BLUFI release failed: %s\n", esp_err_to_name(ret));
        }
    }

    if (s_wifi_state.is_got_ip) {
        ret = app_start_runtime_services();
        if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
            BLUFI_ERROR("Deferred runtime service start failed: %s\n", esp_err_to_name(ret));
        }
    }

    s_runtime_state.transition_task = NULL;
    vTaskDelete(NULL);
}

static void app_schedule_runtime_transition(void)
{
    if (s_runtime_state.transition_task != NULL) {
        return;
    }

    if (xTaskCreate(app_runtime_transition_task,
                    "runtime_transition",
                    EXAMPLE_RUNTIME_TRANSITION_TASK_STACK_SIZE,
                    NULL,
                    EXAMPLE_RUNTIME_TRANSITION_TASK_PRIORITY,
                    &s_runtime_state.transition_task) != pdPASS) {
        s_runtime_state.transition_task = NULL;
        BLUFI_ERROR("Failed to create runtime transition task\n");
    }
}

/* Collects raw runtime state once per second and delegates display mapping to the presenter layer. */
static void app_ui_task(void *arg)
{
    status_presenter_input_t presenter_input = {0};
    display_view_model_t view_model = {0};
    esp_err_t ret = ESP_OK;

    (void)arg;
    BLUFI_INFO("app_ui_task stack_free=%u bytes\n",
               (unsigned int)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));

    while (true) {
        if (time_service_take_sync_notification()) {
            app_sync_rtc_from_system_time();
        }

        memset(&presenter_input, 0, sizeof(presenter_input));
        memset(&view_model, 0, sizeof(view_model));

        presenter_input.wifi_connected = s_wifi_state.is_got_ip;
        presenter_input.time_valid = time_service_has_valid_time();

        if (presenter_input.wifi_connected) {
            presenter_input.wifi_rssi_valid = app_get_wifi_rssi(&presenter_input.wifi_rssi);
        }

        ret = weather_service_get_snapshot(&presenter_input.weather_snapshot);
        presenter_input.weather_snapshot_valid = (ret == ESP_OK);
        if (ret != ESP_OK) {
            BLUFI_ERROR("Weather device provider failed: %s\n", esp_err_to_name(ret));
        }

        if (presenter_input.time_valid) {
            ret = time_service_get_local_time(&presenter_input.current_time);
            if (ret != ESP_OK) {
                BLUFI_ERROR("Failed to get local time for display: %s\n", esp_err_to_name(ret));
                presenter_input.time_valid = false;
                memset(&presenter_input.current_time, 0, sizeof(presenter_input.current_time));
            }
        }

        ret = status_presenter_build_display_model(&presenter_input, &view_model);
        if (ret != ESP_OK) {
            BLUFI_ERROR("Status presenter failed: %s\n", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        app_apply_provisioning_view(&view_model);
        ret = display_service_render(&view_model);
        if (ret != ESP_OK) {
            BLUFI_ERROR("Display render failed: %s\n", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void app_record_wifi_conn_info(int rssi, uint8_t reason)
{
    blufi_service_record_wifi_conn_info(s_wifi_state.connecting, rssi, reason);
}

static void app_wifi_connect(void)
{
    esp_err_t ret = ESP_OK;

    s_wifi_retry = 0;
    ret = esp_wifi_connect();
    s_wifi_state.connecting = (ret == ESP_OK);
    BLUFI_INFO("WiFi connect requested: %s\n", esp_err_to_name(ret));
    if (blufi_service_has_connect_request()) {
        app_provisioning_set_state(ret == ESP_OK ?
                                   DISPLAY_PROVISIONING_STATE_CONNECTING :
                                   DISPLAY_PROVISIONING_STATE_FAILED);
    }
    app_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    blufi_service_notify_wifi_status();
}

static bool app_wifi_reconnect(void)
{
    bool ret;
    if (s_wifi_state.connecting && s_wifi_retry++ < EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY) {
        BLUFI_INFO("BLUFI WiFi starts reconnection\n");
        esp_err_t connect_ret = esp_wifi_connect();
        s_wifi_state.connecting = (connect_ret == ESP_OK);
        BLUFI_INFO("WiFi reconnect requested: %s retry=%u/%u\n",
                   esp_err_to_name(connect_ret),
                   (unsigned int)s_wifi_retry,
                   (unsigned int)EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY);
        app_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
        blufi_service_notify_wifi_status();
        ret = true;
    } else {
        ret = false;
    }
    return ret;
}

static void app_build_blufi_wifi_status(blufi_service_wifi_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->connected = s_wifi_state.connected;
    out_status->is_got_ip = s_wifi_state.is_got_ip;
    out_status->connecting = s_wifi_state.connecting;
    memcpy(out_status->bssid, s_wifi_state.bssid, sizeof(out_status->bssid));
    memcpy(out_status->ssid, s_wifi_state.ssid, sizeof(out_status->ssid));
    out_status->ssid_len = s_wifi_state.ssid_len;
}

static void app_ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        blufi_service_wifi_status_t status;
        esp_err_t ret = ESP_OK;

        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        s_wifi_state.is_got_ip = true;
        BLUFI_INFO("WiFi got IP\n");
        app_log_internal_heap("got_ip");
        if ((s_provisioning_ui_state.state != DISPLAY_PROVISIONING_STATE_HIDDEN) ||
            blufi_service_has_connect_request()) {
            app_provisioning_set_state(DISPLAY_PROVISIONING_STATE_SUCCESS);
        }
        app_build_blufi_wifi_status(&status);
        blufi_service_on_wifi_got_ip(&status);
        ret = time_service_start_sntp();
        if (ret != ESP_OK) {
            BLUFI_ERROR("Failed to start SNTP: %s\n", esp_err_to_name(ret));
        }
        app_schedule_runtime_transition();
        break;
    }
    default:
        break;
    }
    return;
}

static void app_wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_event_sta_connected_t *event;
    wifi_event_sta_disconnected_t *disconnected_event;

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        if (!s_wifi_state.has_stored_config && !blufi_service_has_connect_request()) {
            BLUFI_INFO("Waiting for BLUFI WiFi credentials before STA connect\n");
            break;
        }
        app_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        s_wifi_state.connected = true;
        s_wifi_state.connecting = false;
        event = (wifi_event_sta_connected_t*) event_data;
        memcpy(s_wifi_state.bssid, event->bssid, 6);
        memcpy(s_wifi_state.ssid, event->ssid, event->ssid_len);
        s_wifi_state.ssid_len = event->ssid_len;
        BLUFI_INFO("WiFi STA connected: ssid_len=%d channel=%d authmode=%d\n",
                   event->ssid_len,
                   event->channel,
                   event->authmode);
        app_log_internal_heap("wifi_sta_connected");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        disconnected_event = (wifi_event_sta_disconnected_t*) event_data;
        BLUFI_INFO("WiFi STA disconnected: reason=%d(%s) rssi=%d was_connected=%d was_connecting=%d retry=%u/%u\n",
                   disconnected_event->reason,
                   app_wifi_reason_name(disconnected_event->reason),
                   disconnected_event->rssi,
                   s_wifi_state.connected ? 1 : 0,
                   s_wifi_state.connecting ? 1 : 0,
                   (unsigned int)s_wifi_retry,
                   (unsigned int)EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY);
        /* Stop retrying only while we are still in the association attempt phase */
        if (s_wifi_state.connecting && !app_wifi_reconnect()) {
            s_wifi_state.connecting = false;
            app_record_wifi_conn_info(disconnected_event->rssi, disconnected_event->reason);
            if (blufi_service_has_connect_request()) {
                app_provisioning_set_state(DISPLAY_PROVISIONING_STATE_FAILED);
            }
            blufi_service_notify_wifi_status();
            if (blufi_service_can_start()) {
                BLUFI_INFO("Stored WiFi connection failed; starting BLUFI for reprovisioning\n");
                app_prepare_provisioning_qr_payload();
                app_provisioning_set_state(DISPLAY_PROVISIONING_STATE_QR);
                (void)blufi_service_start();
            }
        }
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        s_wifi_state.connected = false;
        s_wifi_state.is_got_ip = false;
        memset(s_wifi_state.ssid, 0, 32);
        memset(s_wifi_state.bssid, 0, 6);
        s_wifi_state.ssid_len = 0;
        blufi_service_on_wifi_disconnected();
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    case WIFI_EVENT_SCAN_DONE:
        blufi_service_send_wifi_list_from_scan();
        break;

    default:
        break;
    }
    return;
}

#if CONFIG_APP_FORCE_BLUFI_PROVISIONING_ON_BOOT
static esp_err_t app_clear_wifi_credentials_for_blufi_test(void)
{
    esp_err_t ret = ESP_OK;

    /* Temporary BLUFI app test hook: remove after provisioning validation. */
    BLUFI_INFO("Temporary BLUFI test: clearing stored WiFi config\n");
    ret = esp_wifi_restore();
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to clear stored WiFi config: %s\n", esp_err_to_name(ret));
        return ret;
    }

    blufi_service_reset_wifi_config_cache();
    return ESP_OK;
}
#endif

static void app_start_connectivity(void)
{
    wifi_config_t stored_sta_config = {0};
    esp_err_t config_ret = ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_ip_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    app_log_internal_heap("before_esp_wifi_init");
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    app_log_internal_heap("after_esp_wifi_init");
#if CONFIG_APP_FORCE_BLUFI_PROVISIONING_ON_BOOT
    ESP_ERROR_CHECK( app_clear_wifi_credentials_for_blufi_test() );
#endif
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    app_prepare_provisioning_qr_payload();
    config_ret = esp_wifi_get_config(WIFI_IF_STA, &stored_sta_config);
    s_wifi_state.has_stored_config = (config_ret == ESP_OK) && (stored_sta_config.sta.ssid[0] != '\0');
    if (s_wifi_state.has_stored_config) {
        blufi_service_set_sta_config(&stored_sta_config);
        app_provisioning_set_state(DISPLAY_PROVISIONING_STATE_HIDDEN);
        BLUFI_INFO("Stored WiFi config found; skipping BLUFI startup\n");
    } else {
        BLUFI_INFO("No stored WiFi config; starting BLUFI provisioning\n");
        app_provisioning_set_state(DISPLAY_PROVISIONING_STATE_QR);
        (void)blufi_service_start();
    }
    app_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    app_log_internal_heap("before_esp_wifi_start");
    ESP_ERROR_CHECK( esp_wifi_start() );
    app_log_internal_heap("after_esp_wifi_start");
}

static esp_err_t app_lifecycle_init_core_services(void)
{
    esp_err_t ret = ESP_OK;

    ret = button_service_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = device_cloud_service_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = network_task_service_init();
    if (ret != ESP_OK) {
        return ret;
    }
    network_task_service_register_startup_pull_done_handler(app_startup_pull_done, NULL);
    network_task_service_register_startup_weather_done_handler(app_startup_weather_done, NULL);

    ret = storage_service_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_cache_service_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_service_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = time_service_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = weather_service_prepare();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = playback_task_service_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = rtc_service_init();
    if (ret != ESP_OK) {
        BLUFI_ERROR("RTC service init failed: %s\n", esp_err_to_name(ret));
    } else {
        app_sync_time_from_rtc();
    }

    return ESP_OK;
}

static bool app_blufi_get_wifi_status(void *ctx, blufi_service_wifi_status_t *out_status)
{
    (void)ctx;

    if (out_status == NULL) {
        return false;
    }

    app_build_blufi_wifi_status(out_status);
    return true;
}

static void app_blufi_schedule_runtime_transition(void *ctx)
{
    (void)ctx;
    app_schedule_runtime_transition();
}

static void app_blufi_request_wifi_connect(void *ctx)
{
    (void)ctx;
    app_wifi_connect();
}

static void app_blufi_request_wifi_disconnect(void *ctx)
{
    (void)ctx;
    esp_wifi_disconnect();
}

static void app_blufi_request_startup_playback_pull(void *ctx)
{
    (void)ctx;

    if (!s_wifi_state.is_got_ip) {
        BLUFI_INFO("Skip BLUFI startup playback pull because WiFi has no IP\n");
        return;
    }
    if (!s_runtime_state.startup_pull_done && s_runtime_state.network_started) {
        s_runtime_state.startup_pull_requested = true;
        network_task_service_request_playback_pull(NETWORK_TASK_PLAYBACK_REASON_STARTUP);
    }
}

static void app_blufi_request_startup_weather_refresh(void *ctx)
{
    (void)ctx;

    if (!s_wifi_state.is_got_ip) {
        BLUFI_INFO("Skip BLUFI startup weather refresh because WiFi has no IP\n");
        return;
    }
    if (!s_runtime_state.startup_weather_done && s_runtime_state.network_started) {
        s_runtime_state.startup_weather_requested = true;
        network_task_service_request_weather_refresh(NETWORK_TASK_WEATHER_REASON_STARTUP);
    }
}

static void app_blufi_request_weather_refresh(void *ctx)
{
    (void)ctx;
    if (!s_wifi_state.is_got_ip) {
        BLUFI_INFO("Skip BLUFI weather refresh request because WiFi has no IP\n");
        return;
    }
    weather_service_request_refresh();
}

static void app_blufi_request_playback_sync(void *ctx)
{
    (void)ctx;
    if (!s_wifi_state.is_got_ip) {
        BLUFI_INFO("Skip BLUFI playback sync request because WiFi has no IP\n");
        return;
    }
    playback_task_service_request_sync();
}

static void app_blufi_cloud_config_changed(void *ctx)
{
    (void)ctx;

    if (!s_wifi_state.is_got_ip) {
        BLUFI_INFO("Skip BLUFI cloud config sync because WiFi has no IP\n");
        return;
    }

    if (!s_runtime_state.startup_pull_done && s_runtime_state.network_started) {
        app_blufi_request_startup_playback_pull(NULL);
    } else if (!s_runtime_state.startup_weather_done && s_runtime_state.network_started) {
        app_blufi_request_startup_weather_refresh(NULL);
    } else {
        app_blufi_request_weather_refresh(NULL);
        app_blufi_request_playback_sync(NULL);
    }
}

esp_err_t app_lifecycle_boot(void)
{
    esp_err_t ret = ESP_OK;
    const blufi_service_runtime_hooks_t blufi_hooks = {
        .get_wifi_status = app_blufi_get_wifi_status,
        .schedule_runtime_transition = app_blufi_schedule_runtime_transition,
        .request_wifi_connect = app_blufi_request_wifi_connect,
        .request_wifi_disconnect = app_blufi_request_wifi_disconnect,
        .request_startup_playback_pull = app_blufi_request_startup_playback_pull,
        .request_startup_weather_refresh = app_blufi_request_startup_weather_refresh,
        .request_weather_refresh = app_blufi_request_weather_refresh,
        .request_playback_sync = app_blufi_request_playback_sync,
        .cloud_config_changed = app_blufi_cloud_config_changed,
    };

    ret = app_lifecycle_init_core_services();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = blufi_service_init(&blufi_hooks, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = app_start_display_services("startup");
    if (ret != ESP_OK) {
        BLUFI_ERROR("Display startup failed: %s\n", esp_err_to_name(ret));
    }

    app_start_connectivity();

    return ESP_OK;
}
