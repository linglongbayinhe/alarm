/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#ifndef BLUFI_SERVICE_H
#define BLUFI_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

typedef struct {
    bool connected;
    bool got_ip;
    bool connecting;
    uint8_t bssid[6];
    uint8_t ssid[32];
    int ssid_len;
} blufi_service_wifi_status_t;

typedef struct {
    bool (*get_wifi_status)(void *ctx, blufi_service_wifi_status_t *out_status);
    void (*schedule_runtime_transition)(void *ctx);
    void (*request_wifi_connect)(void *ctx);
    void (*request_wifi_disconnect)(void *ctx);
    void (*request_startup_playback_pull)(void *ctx);
    void (*request_startup_weather_refresh)(void *ctx);
    void (*request_weather_refresh)(void *ctx);
    void (*request_playback_sync)(void *ctx);
    void (*cloud_config_changed)(void *ctx);
} blufi_service_runtime_hooks_t;

esp_err_t blufi_service_init(const blufi_service_runtime_hooks_t *hooks, void *ctx);
esp_err_t blufi_service_start(void);
esp_err_t blufi_service_release_if_ready(void);

bool blufi_service_is_runtime_blocked(void);
bool blufi_service_can_start(void);
bool blufi_service_has_connect_request(void);

void blufi_service_reset_wifi_config_cache(void);
void blufi_service_set_sta_config(const wifi_config_t *config);
void blufi_service_record_wifi_conn_info(bool connecting, int rssi, uint8_t reason);
void blufi_service_on_wifi_got_ip(const blufi_service_wifi_status_t *status);
void blufi_service_on_wifi_disconnected(void);
void blufi_service_send_wifi_status_report(const blufi_service_wifi_status_t *status);
void blufi_service_send_wifi_list_from_scan(void);

#endif
