/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */


#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif

#include "app_lifecycle.h"
#include "esp_blufi_api.h"
#include "blufi_example.h"

#include "esp_blufi.h"
#include "audio_cache_service.h"
#include "audio_service.h"
#include "device_cloud_service.h"
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

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

static void app_blufi_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);
static void app_ui_task(void *arg);
static esp_err_t app_start_blufi_services(void);
static esp_err_t app_start_display_services(const char *reason);
static void app_schedule_runtime_transition(void);
static void app_startup_pull_done(esp_err_t ret, void *ctx);
static void app_startup_weather_done(esp_err_t ret, void *ctx);

static esp_blufi_callbacks_t app_blufi_callbacks = {
    .event_cb = app_blufi_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

#define WIFI_LIST_NUM   10
#define EXAMPLE_UI_TASK_STACK_SIZE 3072
#define EXAMPLE_UI_TASK_PRIORITY   5
#define EXAMPLE_RUNTIME_TRANSITION_TASK_STACK_SIZE 4096
#define EXAMPLE_RUNTIME_TRANSITION_TASK_PRIORITY   4

static wifi_config_t sta_config;
static wifi_config_t ap_config;

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static uint8_t s_wifi_retry = 0;

typedef struct {
    bool connected;
    bool got_ip;
    bool connecting;
    bool ssid_received;
    bool password_received;
    bool connect_requested;
    bool has_stored_config;
    uint8_t bssid[6];
    uint8_t ssid[32];
    int ssid_len;
    wifi_sta_list_t sta_list;
    esp_blufi_extra_info_t conn_info;
} app_wifi_state_t;

typedef struct {
    bool ble_connected;
    bool active;
    bool starting;
    bool resources_released;
} app_blufi_state_t;

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

static app_wifi_state_t s_wifi_state;
static app_blufi_state_t s_blufi_state;
static app_runtime_state_t s_runtime_state;
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

    if ((rssi_out == NULL) || !s_wifi_state.got_ip) {
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

static esp_err_t app_release_blufi_resources(void)
{
    esp_err_t ret = ESP_OK;

    if (s_blufi_state.resources_released || !s_blufi_state.active) {
        return ESP_OK;
    }
    if (s_blufi_state.ble_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    app_log_internal_heap("before_blufi_release");
    esp_blufi_adv_stop();
    blufi_security_deinit();

    ret = esp_blufi_host_deinit();
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to deinit BLUFI host: %s\n", esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_deinit();
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to deinit BLUFI controller: %s\n", esp_err_to_name(ret));
        return ret;
    }
#if CONFIG_IDF_TARGET_ESP32
    ret = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        BLUFI_ERROR("Failed to release BLE memory: %s\n", esp_err_to_name(ret));
        return ret;
    }
#endif
#endif

    s_blufi_state.active = false;
    s_blufi_state.resources_released = true;
    app_log_internal_heap("after_blufi_release");
    return ESP_OK;
}

static esp_err_t app_start_blufi_services(void)
{
    esp_err_t ret = ESP_OK;

    if (s_blufi_state.active || s_blufi_state.starting) {
        return ESP_OK;
    }
    if (s_blufi_state.resources_released) {
        BLUFI_ERROR("BLUFI memory was already released; restart is required for BLUFI\n");
        return ESP_ERR_INVALID_STATE;
    }

    s_blufi_state.starting = true;
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    app_log_internal_heap("before_blufi_controller_init");
    ret = esp_blufi_controller_init();
    if (ret != ESP_OK) {
        s_blufi_state.starting = false;
        BLUFI_ERROR("BLUFI controller init failed: %s\n", esp_err_to_name(ret));
        return ret;
    }
#endif

    app_log_internal_heap("before_blufi_host_init");
    ret = esp_blufi_host_and_cb_init(&app_blufi_callbacks);
    if (ret != ESP_OK) {
        s_blufi_state.starting = false;
        BLUFI_ERROR("BLUFI host init failed: %s\n", esp_err_to_name(ret));
        return ret;
    }
    app_log_internal_heap("after_blufi_host_init");
    BLUFI_INFO("BLUFI VERSION %04x\n", esp_blufi_get_version());
    return ESP_OK;
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
    if (!s_wifi_state.got_ip) {
        return ESP_ERR_INVALID_STATE;
    }
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    if (s_blufi_state.active && !s_blufi_state.resources_released) {
        BLUFI_INFO("Deferring runtime service start until BLUFI memory is released\n");
        return ESP_ERR_INVALID_STATE;
    }
#endif

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

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    if (s_wifi_state.got_ip && s_blufi_state.active && !s_blufi_state.ble_connected && !s_blufi_state.resources_released) {
        ret = app_release_blufi_resources();
        if (ret != ESP_OK) {
            BLUFI_ERROR("Deferred BLUFI release failed: %s\n", esp_err_to_name(ret));
        }
    }
#endif

    if (s_wifi_state.got_ip) {
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

        presenter_input.wifi_connected = s_wifi_state.got_ip;
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

        ret = display_service_render(&view_model);
        if (ret != ESP_OK) {
            BLUFI_ERROR("Display render failed: %s\n", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void app_record_wifi_conn_info(int rssi, uint8_t reason)
{
    memset(&s_wifi_state.conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (s_wifi_state.connecting) {
        s_wifi_state.conn_info.sta_max_conn_retry_set = true;
        s_wifi_state.conn_info.sta_max_conn_retry = EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY;
    } else {
        s_wifi_state.conn_info.sta_conn_rssi_set = true;
        s_wifi_state.conn_info.sta_conn_rssi = rssi;
        s_wifi_state.conn_info.sta_conn_end_reason_set = true;
        s_wifi_state.conn_info.sta_conn_end_reason = reason;
    }
}

static void app_wifi_connect(void)
{
    esp_err_t ret = ESP_OK;

    s_wifi_retry = 0;
    ret = esp_wifi_connect();
    s_wifi_state.connecting = (ret == ESP_OK);
    BLUFI_INFO("WiFi connect requested: %s\n", esp_err_to_name(ret));
    app_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
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
        ret = true;
    } else {
        ret = false;
    }
    return ret;
}

static int app_softap_get_current_connection_number(void)
{
    esp_err_t ret;
    ret = esp_wifi_ap_get_sta_list(&s_wifi_state.sta_list);
    if (ret == ESP_OK)
    {
        return s_wifi_state.sta_list.num;
    }

    return 0;
}

static void app_ip_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_mode_t mode;

    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        esp_blufi_extra_info_t info;
        esp_err_t ret = ESP_OK;

        xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
        esp_wifi_get_mode(&mode);

        memset(&info, 0, sizeof(esp_blufi_extra_info_t));
        memcpy(info.sta_bssid, s_wifi_state.bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid = s_wifi_state.ssid;
        info.sta_ssid_len = s_wifi_state.ssid_len;
        s_wifi_state.got_ip = true;
        BLUFI_INFO("WiFi got IP\n");
        app_log_internal_heap("got_ip");
        if (s_blufi_state.ble_connected == true) {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, app_softap_get_current_connection_number(), &info);
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
            app_schedule_runtime_transition();
        }
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
    wifi_mode_t mode;

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        if (!s_wifi_state.has_stored_config && !s_wifi_state.connect_requested) {
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
        /* Only handle reconnection during connecting */
        if (s_wifi_state.connected == false && app_wifi_reconnect() == false) {
            s_wifi_state.connecting = false;
            app_record_wifi_conn_info(disconnected_event->rssi, disconnected_event->reason);
            if (!s_blufi_state.active && !s_blufi_state.starting && !s_blufi_state.resources_released) {
                BLUFI_INFO("Stored WiFi connection failed; starting BLUFI for reprovisioning\n");
                (void)app_start_blufi_services();
            }
        }
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        s_wifi_state.connected = false;
        s_wifi_state.got_ip = false;
        memset(s_wifi_state.ssid, 0, 32);
        memset(s_wifi_state.bssid, 0, 6);
        s_wifi_state.ssid_len = 0;
        xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
        break;
    case WIFI_EVENT_AP_START:
        esp_wifi_get_mode(&mode);

        /* TODO: get config or information of softap, then set to report extra_info */
        if (s_blufi_state.ble_connected == true) {
            if (s_wifi_state.connected) {
                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, s_wifi_state.bssid, 6);
                info.sta_bssid_set = true;
                info.sta_ssid = s_wifi_state.ssid;
                info.sta_ssid_len = s_wifi_state.ssid_len;
                esp_blufi_send_wifi_conn_report(mode, s_wifi_state.got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, app_softap_get_current_connection_number(), &info);
            } else if (s_wifi_state.connecting) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, app_softap_get_current_connection_number(), &s_wifi_state.conn_info);
            } else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, app_softap_get_current_connection_number(), &s_wifi_state.conn_info);
            }
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }
        break;
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t apCount = 0;
        esp_wifi_scan_get_ap_num(&apCount);
        if (apCount == 0) {
            BLUFI_INFO("Nothing AP found");
            break;
        }
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * apCount);
        if (!ap_list) {
            BLUFI_ERROR("malloc error, ap_list is NULL");
            esp_wifi_clear_ap_list();
            break;
        }
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
        esp_blufi_ap_record_t * blufi_ap_list = (esp_blufi_ap_record_t *)malloc(apCount * sizeof(esp_blufi_ap_record_t));
        if (!blufi_ap_list) {
            if (ap_list) {
                free(ap_list);
            }
            BLUFI_ERROR("malloc error, blufi_ap_list is NULL");
            break;
        }
        for (int i = 0; i < apCount; ++i)
        {
            blufi_ap_list[i].rssi = ap_list[i].rssi;
            memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
        }

        if (s_blufi_state.ble_connected == true) {
            esp_blufi_send_wifi_list(apCount, blufi_ap_list);
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet\n");
        }

        esp_wifi_scan_stop();
        free(ap_list);
        free(blufi_ap_list);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        BLUFI_INFO("station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        BLUFI_INFO("station "MACSTR" leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
        break;
    }

    default:
        break;
    }
    return;
}

static void app_start_connectivity(void)
{
    wifi_config_t stored_sta_config = {0};
    esp_err_t config_ret = ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_ip_event_handler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    app_log_internal_heap("before_esp_wifi_init");
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    app_log_internal_heap("after_esp_wifi_init");
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    config_ret = esp_wifi_get_config(WIFI_IF_STA, &stored_sta_config);
    s_wifi_state.has_stored_config = (config_ret == ESP_OK) && (stored_sta_config.sta.ssid[0] != '\0');
    if (s_wifi_state.has_stored_config) {
        sta_config = stored_sta_config;
        BLUFI_INFO("Stored WiFi config found; skipping BLUFI startup\n");
    } else {
        BLUFI_INFO("No stored WiFi config; starting BLUFI provisioning\n");
        (void)app_start_blufi_services();
    }
    app_record_wifi_conn_info(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    app_log_internal_heap("before_esp_wifi_start");
    ESP_ERROR_CHECK( esp_wifi_start() );
    app_log_internal_heap("after_esp_wifi_start");
}

static esp_err_t app_lifecycle_init_core_services(void)
{
    esp_err_t ret = ESP_OK;

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

static void app_blufi_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    /* BLUFI callbacks run the provisioning actions directly to preserve existing behavior. */
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        BLUFI_INFO("BLUFI init finish\n");
        s_blufi_state.starting = false;
        s_blufi_state.active = true;
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        BLUFI_INFO("BLUFI deinit finish\n");
        s_blufi_state.starting = false;
        s_blufi_state.active = false;
        s_blufi_state.resources_released = true;
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        BLUFI_INFO("BLUFI ble connect\n");
        s_blufi_state.ble_connected = true;
        esp_blufi_adv_stop();
        blufi_security_init();
        #ifdef CONFIG_EXAMPLE_BLUFI_BLE_SMP_ENABLE
        // Try to initiate BLE security request after connection established.
        BLUFI_INFO("Try to initiate BLE security request\n");
        esp_err_t ret = esp_blufi_start_security_request(param->connect.remote_bda);
        if (ret != ESP_OK) {
            BLUFI_ERROR("Failed to start security request: %s\n", esp_err_to_name(ret));
        }
        #endif // CONFIG_EXAMPLE_BLUFI_BLE_SMP_ENABLE
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        BLUFI_INFO("BLUFI ble disconnect\n");
        s_blufi_state.ble_connected = false;
        blufi_security_deinit();
        if (s_wifi_state.got_ip) {
            app_schedule_runtime_transition();
        } else {
            esp_blufi_adv_start();
        }
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        BLUFI_INFO("BLUFI Set WIFI opmode %d\n", param->wifi_mode.op_mode);
        ESP_ERROR_CHECK( esp_wifi_set_mode(param->wifi_mode.op_mode) );
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        s_wifi_state.connect_requested = true;
        BLUFI_INFO("BLUFI request wifi connect to AP (ssid_received=%d password_received=%d ssid_len=%d)\n",
                   s_wifi_state.ssid_received ? 1 : 0,
                   s_wifi_state.password_received ? 1 : 0,
                   s_wifi_state.ssid_len);
        app_log_internal_heap("req_connect_to_ap");
        /* there is no wifi callback when the device has already connected to this wifi
        so disconnect wifi before connection.
        */
        esp_wifi_disconnect();
        app_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        BLUFI_INFO("BLUFI request wifi disconnect from AP\n");
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        BLUFI_ERROR("BLUFI report error, error code %d\n", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
        wifi_mode_t mode;
        esp_blufi_extra_info_t info;

        esp_wifi_get_mode(&mode);

        if (s_wifi_state.connected) {
            memset(&info, 0, sizeof(esp_blufi_extra_info_t));
            memcpy(info.sta_bssid, s_wifi_state.bssid, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = s_wifi_state.ssid;
            info.sta_ssid_len = s_wifi_state.ssid_len;
            esp_blufi_send_wifi_conn_report(mode, s_wifi_state.got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, app_softap_get_current_connection_number(), &info);
        } else if (s_wifi_state.connecting) {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, app_softap_get_current_connection_number(), &s_wifi_state.conn_info);
        } else {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, app_softap_get_current_connection_number(), &s_wifi_state.conn_info);
        }
        BLUFI_INFO("BLUFI get wifi status from AP (got_ip=%d connected=%d connecting=%d ssid_received=%d password_received=%d connect_requested=%d)\n",
                   s_wifi_state.got_ip ? 1 : 0,
                   s_wifi_state.connected ? 1 : 0,
                   s_wifi_state.connecting ? 1 : 0,
                   s_wifi_state.ssid_received ? 1 : 0,
                   s_wifi_state.password_received ? 1 : 0,
                   s_wifi_state.connect_requested ? 1 : 0);
        app_log_internal_heap("get_wifi_status");

        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        BLUFI_INFO("blufi close a gatt connection");
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        /* TODO */
        break;
	case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        sta_config.sta.bssid_set = 1;
        esp_err_t bssid_set_config_ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA BSSID, set_config=%s\n", esp_err_to_name(bssid_set_config_ret));
        break;
	case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len >= sizeof(sta_config.sta.ssid)/sizeof(sta_config.sta.ssid[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid STA SSID\n");
            break;
        }
        strncpy((char *)sta_config.sta.ssid, (char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        s_wifi_state.ssid_received = true;
        esp_err_t ssid_set_config_ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA SSID len=%u value=%s set_config=%s\n",
                   (unsigned int)param->sta_ssid.ssid_len,
                   sta_config.sta.ssid,
                   esp_err_to_name(ssid_set_config_ret));
        app_log_internal_heap("recv_sta_ssid");
        break;
	case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len >= sizeof(sta_config.sta.password)/sizeof(sta_config.sta.password[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid STA PASSWORD\n");
            break;
        }
        strncpy((char *)sta_config.sta.password, (char *)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
        sta_config.sta.threshold.authmode = EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD;
        s_wifi_state.password_received = true;
        esp_err_t passwd_set_config_ret = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA PASSWORD len=%u threshold_authmode=%d set_config=%s (hidden)\n",
                   (unsigned int)param->sta_passwd.passwd_len,
                   sta_config.sta.threshold.authmode,
                   esp_err_to_name(passwd_set_config_ret));
        app_log_internal_heap("recv_sta_password");
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
        if (param->softap_ssid.ssid_len >= sizeof(ap_config.ap.ssid)/sizeof(ap_config.ap.ssid[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid SOFTAP SSID\n");
            break;
        }
        strncpy((char *)ap_config.ap.ssid, (char *)param->softap_ssid.ssid, param->softap_ssid.ssid_len);
        ap_config.ap.ssid[param->softap_ssid.ssid_len] = '\0';
        ap_config.ap.ssid_len = param->softap_ssid.ssid_len;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP SSID %s, ssid len %d\n", ap_config.ap.ssid, ap_config.ap.ssid_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
        if (param->softap_passwd.passwd_len >= sizeof(ap_config.ap.password)/sizeof(ap_config.ap.password[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid SOFTAP PASSWD\n");
            break;
        }
        strncpy((char *)ap_config.ap.password, (char *)param->softap_passwd.passwd, param->softap_passwd.passwd_len);
        ap_config.ap.password[param->softap_passwd.passwd_len] = '\0';
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP PASSWORD len = %d (hidden)\n", param->softap_passwd.passwd_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
        if (param->softap_max_conn_num.max_conn_num > 4) {
            return;
        }
        ap_config.ap.max_connection = param->softap_max_conn_num.max_conn_num;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP MAX CONN NUM %d\n", ap_config.ap.max_connection);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
        if (param->softap_auth_mode.auth_mode >= WIFI_AUTH_MAX) {
            return;
        }
        ap_config.ap.authmode = param->softap_auth_mode.auth_mode;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP AUTH MODE %d\n", ap_config.ap.authmode);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
        if (param->softap_channel.channel > 13) {
            return;
        }
        ap_config.ap.channel = param->softap_channel.channel;
        esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP CHANNEL %d\n", ap_config.ap.channel);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:{
        wifi_scan_config_t scanConf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false
        };
        esp_err_t ret = esp_wifi_scan_start(&scanConf, true);
        if (ret != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        BLUFI_INFO("Recv Custom Data %" PRIu32 "\n", param->custom_data.data_len);
#if CONFIG_APP_LOG_SENSITIVE_DATA
        ESP_LOG_BUFFER_HEX("Custom Data", param->custom_data.data, param->custom_data.data_len);
#else
        BLUFI_INFO("Custom data payload hidden\n");
#endif
        if ((param->custom_data.data != NULL) && (param->custom_data.data_len > 0)) {
            char *payload = calloc(1, param->custom_data.data_len + 1);
            if (payload == NULL) {
                BLUFI_ERROR("Failed to allocate custom data payload buffer\n");
                break;
            }
            memcpy(payload, param->custom_data.data, param->custom_data.data_len);
            payload[param->custom_data.data_len] = '\0';

            bool changed = false;
            esp_err_t custom_data_ret = device_cloud_service_update_from_json(payload,
                                                                              param->custom_data.data_len,
                                                                              &changed);
            if (custom_data_ret != ESP_OK) {
                BLUFI_ERROR("Failed to apply cloud config from custom data: %s\n",
                            esp_err_to_name(custom_data_ret));
            } else if (changed) {
                BLUFI_INFO("Cloud config updated from BLUFI custom data\n");
                if (!s_runtime_state.startup_pull_done && s_runtime_state.network_started) {
                    s_runtime_state.startup_pull_requested = true;
                    network_task_service_request_playback_pull(NETWORK_TASK_PLAYBACK_REASON_STARTUP);
                } else if (!s_runtime_state.startup_weather_done && s_runtime_state.network_started) {
                    s_runtime_state.startup_weather_requested = true;
                    network_task_service_request_weather_refresh(NETWORK_TASK_WEATHER_REASON_STARTUP);
                } else {
                    weather_service_request_refresh();
                    playback_task_service_request_sync();
                }
            }
            free(payload);
        }
        break;
	case ESP_BLUFI_EVENT_RECV_USERNAME:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CA_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
        /* Not handle currently */
        break;;
	case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
        /* Not handle currently */
        break;
    default:
        break;
    }
}

esp_err_t app_lifecycle_boot(void)
{
    esp_err_t ret = ESP_OK;

    ret = app_lifecycle_init_core_services();
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
