/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include "blufi_service.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "blufi_port.h"
#include "device_cloud_service.h"
#include "device_utils.h"

#define EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY CONFIG_EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY

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

#define BLUFI_SOFTAP_CONNECTION_COUNT 0
#define BLUFI_SUCCESS_DISCONNECT_DELAY_MS 500
#define BLUFI_DEVICE_INFO_DELAY_MS 100
#define BLUFI_DISCONNECT_TASK_STACK_SIZE 2048
#define BLUFI_DEVICE_INFO_TASK_STACK_SIZE 3072
#define BLUFI_DISCONNECT_TASK_PRIORITY 4
#define BLUFI_DEVICE_INFO_TASK_PRIORITY 4
#define BLUFI_DEVICE_INFO_PAYLOAD_SIZE 128

typedef struct {
    bool ble_connected;
    bool active;
    bool starting;
    bool resources_released;
    bool ssid_received;
    bool password_received;
    bool connect_requested;
    bool wifi_got_ip;
    bool pending_wifi_report;
    bool success_disconnect_pending;
    bool device_info_sent;
    bool device_info_task_pending;
    esp_blufi_extra_info_t conn_info;
} blufi_service_state_t;

static void blufi_service_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);

static esp_blufi_callbacks_t s_blufi_callbacks = {
    .event_cb = blufi_service_event_callback,
    .negotiate_data_handler = blufi_dh_negotiate_data_handler,
    .encrypt_func = blufi_aes_encrypt,
    .decrypt_func = blufi_aes_decrypt,
    .checksum_func = blufi_crc_checksum,
};

static blufi_service_runtime_hooks_t s_hooks;
static void *s_hook_ctx;
static blufi_service_state_t s_state;
static wifi_config_t s_sta_config;

static void blufi_service_log_internal_heap(const char *label);

static void blufi_service_send_device_info_task(void *arg)
{
    (void)arg;

    char device_id[DEVICE_ID_SIZE] = {0};
    char payload[BLUFI_DEVICE_INFO_PAYLOAD_SIZE] = {0};
    int written = 0;
    esp_err_t ret = ESP_OK;

    vTaskDelay(pdMS_TO_TICKS(BLUFI_DEVICE_INFO_DELAY_MS));
    s_state.device_info_task_pending = false;

    if (!s_state.ble_connected || s_state.device_info_sent) {
        vTaskDelete(NULL);
        return;
    }

    ret = device_utils_get_device_id(device_id, sizeof(device_id));
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to get device id for BLUFI deviceInfo: %s\n", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    written = snprintf(payload,
                       sizeof(payload),
                       "{\"type\":\"deviceInfo\",\"deviceId\":\"%s\",\"bleName\":\"%s\"}",
                       device_id,
                       CUSTOM_BLUFI_DEVICE_NAME);
    if ((written < 0) || ((size_t)written >= sizeof(payload))) {
        BLUFI_ERROR("BLUFI deviceInfo payload is too large\n");
        vTaskDelete(NULL);
        return;
    }

    ret = esp_blufi_send_custom_data((uint8_t *)payload, (uint32_t)strlen(payload));
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to send BLUFI deviceInfo: %s\n", esp_err_to_name(ret));
    } else {
        s_state.device_info_sent = true;
        BLUFI_INFO("BLUFI deviceInfo sent\n");
    }

    vTaskDelete(NULL);
}

static void blufi_service_schedule_device_info(void)
{
    if (!s_state.ble_connected || s_state.device_info_sent || s_state.device_info_task_pending) {
        return;
    }

    s_state.device_info_task_pending = true;
    if (xTaskCreate(blufi_service_send_device_info_task,
                    "blufi_devinfo",
                    BLUFI_DEVICE_INFO_TASK_STACK_SIZE,
                    NULL,
                    BLUFI_DEVICE_INFO_TASK_PRIORITY,
                    NULL) != pdPASS) {
        s_state.device_info_task_pending = false;
        BLUFI_ERROR("Failed to create BLUFI deviceInfo task\n");
    }
}

void blufi_service_on_negotiate_success(void)
{
    blufi_service_schedule_device_info();
}

static void blufi_service_request_wifi_connect_once(const char *reason)
{
    if (s_state.connect_requested) {
        BLUFI_INFO("BLUFI wifi connect already requested; reporting current status after %s\n",
                   reason == NULL ? "wifi_config" : reason);
        blufi_service_notify_wifi_status();
        return;
    }
    if (!s_state.ssid_received || !s_state.password_received) {
        BLUFI_INFO("BLUFI wifi connect deferred after %s (ssid_received=%d password_received=%d)\n",
                   reason == NULL ? "wifi_config" : reason,
                   s_state.ssid_received ? 1 : 0,
                   s_state.password_received ? 1 : 0);
        return;
    }

    s_state.connect_requested = true;
    BLUFI_INFO("BLUFI auto wifi connect after %s\n", reason == NULL ? "wifi_config" : reason);
    blufi_service_log_internal_heap("auto_connect_to_ap");

    if (s_hooks.request_wifi_disconnect != NULL) {
        s_hooks.request_wifi_disconnect(s_hook_ctx);
    } else {
        esp_wifi_disconnect();
    }
    if (s_hooks.request_wifi_connect != NULL) {
        s_hooks.request_wifi_connect(s_hook_ctx);
    } else {
        esp_err_t ret = esp_wifi_connect();
        BLUFI_INFO("WiFi connect requested: %s\n", esp_err_to_name(ret));
        blufi_service_notify_wifi_status();
    }
}

static void blufi_service_success_disconnect_task(void *arg)
{
    (void)arg;

    vTaskDelay(pdMS_TO_TICKS(BLUFI_SUCCESS_DISCONNECT_DELAY_MS));
    if (s_state.ble_connected && s_state.wifi_got_ip) {
        BLUFI_INFO("WiFi provisioning succeeded; disconnecting BLUFI BLE to continue startup\n");
        esp_blufi_disconnect();
    } else {
        s_state.success_disconnect_pending = false;
    }
    vTaskDelete(NULL);
}

static void blufi_service_schedule_success_disconnect(void)
{
    if (s_state.success_disconnect_pending) {
        return;
    }
    s_state.success_disconnect_pending = true;
    if (xTaskCreate(blufi_service_success_disconnect_task,
                    "blufi_disc",
                    BLUFI_DISCONNECT_TASK_STACK_SIZE,
                    NULL,
                    BLUFI_DISCONNECT_TASK_PRIORITY,
                    NULL) != pdPASS) {
        s_state.success_disconnect_pending = false;
        BLUFI_ERROR("Failed to create BLUFI success disconnect task\n");
    }
}

static void blufi_service_log_internal_heap(const char *label)
{
    BLUFI_INFO("Heap %s: free=%u largest=%u\n",
               label,
               (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void blufi_service_reject_unsupported_softap(const char *event_name)
{
    BLUFI_ERROR("%s is unsupported: this firmware only supports STA provisioning\n",
                event_name == NULL ? "SoftAP provisioning" : event_name);
    esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
}

esp_err_t blufi_service_init(const blufi_service_runtime_hooks_t *hooks, void *ctx)
{
    if (hooks != NULL) {
        s_hooks = *hooks;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }
    s_hook_ctx = ctx;
    return ESP_OK;
}

esp_err_t blufi_service_start(void)
{
    esp_err_t ret = ESP_OK;

    if (s_state.active || s_state.starting) {
        return ESP_OK;
    }
    if (s_state.resources_released) {
        BLUFI_ERROR("BLUFI memory was already released; restart is required for BLUFI\n");
        return ESP_ERR_INVALID_STATE;
    }

    s_state.starting = true;
    blufi_service_log_internal_heap("before_blufi_controller_init");
    ret = esp_blufi_controller_init();
    if (ret != ESP_OK) {
        s_state.starting = false;
        BLUFI_ERROR("BLUFI controller init failed: %s\n", esp_err_to_name(ret));
        return ret;
    }

    blufi_service_log_internal_heap("before_blufi_host_init");
    ret = esp_blufi_host_and_cb_init(&s_blufi_callbacks);
    if (ret != ESP_OK) {
        s_state.starting = false;
        BLUFI_ERROR("BLUFI host init failed: %s\n", esp_err_to_name(ret));
        return ret;
    }
    blufi_service_log_internal_heap("after_blufi_host_init");
    BLUFI_INFO("BLUFI VERSION %04x\n", esp_blufi_get_version());
    return ESP_OK;
}

static esp_err_t blufi_service_release(void)
{
    esp_err_t ret = ESP_OK;

    blufi_service_log_internal_heap("before_blufi_release");
    esp_blufi_adv_stop();
    BLUFI_INFO("BLE advertising stopped\n");
    blufi_security_deinit();

    ret = esp_blufi_host_deinit();
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to deinit BLUFI host: %s\n", esp_err_to_name(ret));
        return ret;
    }

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

    BLUFI_INFO("BLE controller released\n");
    s_state.active = false;
    s_state.resources_released = true;
    blufi_service_log_internal_heap("after_blufi_release");
    return ESP_OK;
}

esp_err_t blufi_service_release_if_ready(void)
{
    if (s_state.resources_released || !s_state.active) {
        return ESP_OK;
    }
    if (s_state.ble_connected) {
        return ESP_OK;
    }
    return blufi_service_release();
}

bool blufi_service_is_runtime_blocked(void)
{
    return s_state.active && !s_state.resources_released;
}

bool blufi_service_can_start(void)
{
    return !s_state.active && !s_state.starting && !s_state.resources_released;
}

bool blufi_service_has_connect_request(void)
{
    return s_state.connect_requested;
}

void blufi_service_reset_wifi_config_cache(void)
{
    memset(&s_sta_config, 0, sizeof(s_sta_config));
    s_state.ssid_received = false;
    s_state.password_received = false;
    s_state.connect_requested = false;
    s_state.pending_wifi_report = false;
}

void blufi_service_set_sta_config(const wifi_config_t *config)
{
    if (config == NULL) {
        memset(&s_sta_config, 0, sizeof(s_sta_config));
        return;
    }

    s_sta_config = *config;
}

void blufi_service_record_wifi_conn_info(bool connecting, int rssi, uint8_t reason)
{
    memset(&s_state.conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (connecting) {
        s_state.conn_info.sta_max_conn_retry_set = true;
        s_state.conn_info.sta_max_conn_retry = EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY;
    } else {
        s_state.conn_info.sta_conn_rssi_set = true;
        s_state.conn_info.sta_conn_rssi = rssi;
        s_state.conn_info.sta_conn_end_reason_set = true;
        s_state.conn_info.sta_conn_end_reason = reason;
    }
}

void blufi_service_on_wifi_got_ip(const blufi_service_wifi_status_t *status)
{
    (void)status;

    s_state.wifi_got_ip = true;
    blufi_service_notify_wifi_status();
}

void blufi_service_on_wifi_disconnected(void)
{
    s_state.wifi_got_ip = false;
}

void blufi_service_notify_wifi_status(void)
{
    if (s_hooks.get_wifi_status == NULL) {
        return;
    }
    if (!s_state.ble_connected) {
        s_state.pending_wifi_report = true;
        BLUFI_INFO("Defer Wi-Fi status BLE report until peer connects\n");
        return;
    }
    blufi_service_wifi_status_t status = {0};
    if (s_hooks.get_wifi_status(s_hook_ctx, &status)) {
        s_state.pending_wifi_report = false;
        blufi_service_send_wifi_status_report(&status);
    }
}

void blufi_service_send_wifi_status_report(const blufi_service_wifi_status_t *status)
{
    wifi_mode_t mode;
    esp_blufi_extra_info_t info;
    esp_blufi_extra_info_t *extra_info = &s_state.conn_info;
    esp_blufi_sta_conn_state_t sta_state = ESP_BLUFI_STA_CONN_FAIL;

    if (status == NULL) {
        return;
    }

    memset(&info, 0, sizeof(info));
    esp_wifi_get_mode(&mode);

    if (!s_state.ble_connected) {
        BLUFI_INFO("BLUFI BLE is not connected yet\n");
        return;
    }

    if (status->connected) {
        memcpy(info.sta_bssid, status->bssid, 6);
        info.sta_bssid_set = true;
        info.sta_ssid = (uint8_t *)status->ssid;
        info.sta_ssid_len = status->ssid_len;
        extra_info = &info;
        sta_state = status->got_ip ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP;
    } else if (status->connecting) {
        sta_state = ESP_BLUFI_STA_CONNECTING;
    }

    esp_blufi_send_wifi_conn_report(mode, sta_state, BLUFI_SOFTAP_CONNECTION_COUNT, extra_info);
    if (sta_state == ESP_BLUFI_STA_CONN_SUCCESS) {
        blufi_service_schedule_success_disconnect();
    }
}

void blufi_service_send_wifi_list_from_scan(void)
{
    uint16_t ap_count = 0;

    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        BLUFI_INFO("Nothing AP found");
        return;
    }

    wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_list) {
        BLUFI_ERROR("malloc error, ap_list is NULL");
        esp_wifi_clear_ap_list();
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));

    esp_blufi_ap_record_t *blufi_ap_list = (esp_blufi_ap_record_t *)malloc(ap_count * sizeof(esp_blufi_ap_record_t));
    if (!blufi_ap_list) {
        free(ap_list);
        BLUFI_ERROR("malloc error, blufi_ap_list is NULL");
        return;
    }

    for (int i = 0; i < ap_count; ++i) {
        blufi_ap_list[i].rssi = ap_list[i].rssi;
        memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
    }

    if (s_state.ble_connected == true) {
        esp_blufi_send_wifi_list(ap_count, blufi_ap_list);
    } else {
        BLUFI_INFO("BLUFI BLE is not connected yet\n");
    }

    esp_wifi_scan_stop();
    free(ap_list);
    free(blufi_ap_list);
}

static void blufi_service_handle_custom_data(const uint8_t *data, uint32_t data_len)
{
    BLUFI_INFO("Recv Custom Data %" PRIu32 "\n", data_len);
#if CONFIG_APP_LOG_SENSITIVE_DATA
    ESP_LOG_BUFFER_HEX("Custom Data", data, data_len);
#else
    BLUFI_INFO("Custom data payload hidden\n");
#endif

    if ((data == NULL) || (data_len == 0)) {
        return;
    }

    char *payload = calloc(1, data_len + 1);
    if (payload == NULL) {
        BLUFI_ERROR("Failed to allocate custom data payload buffer\n");
        return;
    }

    memcpy(payload, data, data_len);
    payload[data_len] = '\0';

    bool changed = false;
    esp_err_t ret = device_cloud_service_update_from_json(payload, data_len, &changed);
    if (ret != ESP_OK) {
        BLUFI_ERROR("Failed to apply cloud config from custom data: %s\n", esp_err_to_name(ret));
    } else if (changed) {
        BLUFI_INFO("Cloud config updated from BLUFI custom data\n");
        if (s_hooks.cloud_config_changed != NULL) {
            s_hooks.cloud_config_changed(s_hook_ctx);
        } else {
            if (s_hooks.request_weather_refresh != NULL) {
                s_hooks.request_weather_refresh(s_hook_ctx);
            }
            if (s_hooks.request_playback_sync != NULL) {
                s_hooks.request_playback_sync(s_hook_ctx);
            }
        }
    }

    free(payload);
}

static void blufi_service_event_callback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        BLUFI_INFO("BLUFI init finish\n");
        s_state.starting = false;
        s_state.active = true;
        esp_blufi_adv_start();
        BLUFI_INFO("BLE advertising started: %s\n", CUSTOM_BLUFI_DEVICE_NAME);
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        BLUFI_INFO("BLUFI deinit finish\n");
        s_state.starting = false;
        s_state.active = false;
        s_state.resources_released = true;
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT: {
        BLUFI_INFO("BLUFI ble connect\n");
        s_state.ble_connected = true;
        s_state.device_info_sent = false;
        s_state.device_info_task_pending = false;
        esp_blufi_adv_stop();
        BLUFI_INFO("BLE advertising stopped\n");
        blufi_security_init();
#ifdef CONFIG_EXAMPLE_BLUFI_BLE_SMP_ENABLE
        BLUFI_INFO("Try to initiate BLE security request\n");
        esp_err_t ret = esp_blufi_start_security_request(param->connect.remote_bda);
        if (ret != ESP_OK) {
            BLUFI_ERROR("Failed to start security request: %s\n", esp_err_to_name(ret));
        }
#endif
        if (s_state.pending_wifi_report) {
            blufi_service_notify_wifi_status();
        }
        break;
    }
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        BLUFI_INFO("BLUFI ble disconnect\n");
        s_state.ble_connected = false;
        s_state.success_disconnect_pending = false;
        s_state.device_info_sent = false;
        s_state.device_info_task_pending = false;
        blufi_security_deinit();
        if (s_state.wifi_got_ip) {
            if (s_hooks.schedule_runtime_transition != NULL) {
                s_hooks.schedule_runtime_transition(s_hook_ctx);
            }
        } else {
            esp_blufi_adv_start();
            BLUFI_INFO("BLE advertising started: %s\n", CUSTOM_BLUFI_DEVICE_NAME);
        }
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        BLUFI_INFO("BLUFI Set WIFI opmode %d\n", param->wifi_mode.op_mode);
        if (param->wifi_mode.op_mode != WIFI_MODE_STA) {
            blufi_service_reject_unsupported_softap("BLUFI WiFi opmode AP/APSTA");
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            break;
        }
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        BLUFI_INFO("BLUFI request wifi connect to AP (ssid_received=%d password_received=%d connect_requested=%d)\n",
                   s_state.ssid_received ? 1 : 0,
                   s_state.password_received ? 1 : 0,
                   s_state.connect_requested ? 1 : 0);
        blufi_service_request_wifi_connect_once("req_connect_to_ap");
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        BLUFI_INFO("BLUFI request wifi disconnect from AP\n");
        if (s_hooks.request_wifi_disconnect != NULL) {
            s_hooks.request_wifi_disconnect(s_hook_ctx);
        } else {
            esp_wifi_disconnect();
        }
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        BLUFI_ERROR("BLUFI report error, error code %d\n", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        if (s_hooks.get_wifi_status != NULL) {
            blufi_service_wifi_status_t status = {0};
            if (s_hooks.get_wifi_status(s_hook_ctx, &status)) {
                blufi_service_send_wifi_status_report(&status);
                BLUFI_INFO("BLUFI get WiFi STA status (got_ip=%d connected=%d connecting=%d ssid_received=%d password_received=%d connect_requested=%d)\n",
                           status.got_ip ? 1 : 0,
                           status.connected ? 1 : 0,
                           status.connecting ? 1 : 0,
                           s_state.ssid_received ? 1 : 0,
                           s_state.password_received ? 1 : 0,
                           s_state.connect_requested ? 1 : 0);
                blufi_service_log_internal_heap("get_wifi_status");
            }
        }
        break;
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        BLUFI_INFO("blufi close a gatt connection");
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID: {
        memcpy(s_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        s_sta_config.sta.bssid_set = 1;
        esp_err_t bssid_set_config_ret = esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        BLUFI_INFO("Recv STA BSSID, set_config=%s\n", esp_err_to_name(bssid_set_config_ret));
        break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_SSID: {
        if (param->sta_ssid.ssid_len >= sizeof(s_sta_config.sta.ssid) / sizeof(s_sta_config.sta.ssid[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid STA SSID\n");
            break;
        }
        s_state.password_received = false;
        s_state.connect_requested = false;
        memset(s_sta_config.sta.password, 0, sizeof(s_sta_config.sta.password));
        BLUFI_INFO("BLUFI new STA SSID received; reset password/connect state\n");
        strncpy((char *)s_sta_config.sta.ssid, (char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        s_sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        s_state.ssid_received = true;
        esp_err_t ssid_set_config_ret = esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        BLUFI_INFO("Recv STA SSID len=%u value=%s set_config=%s\n",
                   (unsigned int)param->sta_ssid.ssid_len,
                   s_sta_config.sta.ssid,
                   esp_err_to_name(ssid_set_config_ret));
        blufi_service_log_internal_heap("recv_sta_ssid");
        blufi_service_request_wifi_connect_once("sta_ssid");
        break;
    }
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD: {
        if (param->sta_passwd.passwd_len >= sizeof(s_sta_config.sta.password) / sizeof(s_sta_config.sta.password[0])) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            BLUFI_INFO("Invalid STA PASSWORD\n");
            break;
        }
        strncpy((char *)s_sta_config.sta.password, (char *)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        s_sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
        s_sta_config.sta.threshold.authmode = EXAMPLE_WIFI_SCAN_AUTH_MODE_THRESHOLD;
        s_state.password_received = true;
        esp_err_t passwd_set_config_ret = esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        BLUFI_INFO("Recv STA PASSWORD len=%u threshold_authmode=%d set_config=%s (hidden)\n",
                   (unsigned int)param->sta_passwd.passwd_len,
                   s_sta_config.sta.threshold.authmode,
                   esp_err_to_name(passwd_set_config_ret));
        blufi_service_log_internal_heap("recv_sta_password");
        blufi_service_request_wifi_connect_once("sta_password");
        break;
    }
    case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
        wifi_scan_config_t scan_conf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
        };
        esp_err_t ret = esp_wifi_scan_start(&scan_conf, true);
        if (ret != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        blufi_service_handle_custom_data(param->custom_data.data, param->custom_data.data_len);
        break;
    case ESP_BLUFI_EVENT_RECV_USERNAME:
    case ESP_BLUFI_EVENT_RECV_CA_CERT:
    case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
    case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
    case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
    case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
    default:
        break;
    }
}
