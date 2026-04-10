#include "time_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "TIME_SERVICE";
static const time_t VALID_TIME_THRESHOLD = 1704067200; /* 2024-01-01 00:00:00 UTC */
static const char *SNTP_SERVER = "pool.ntp.org";
static const char *LOCAL_TIMEZONE = "HKT-8";

static volatile bool s_sync_notification_pending;

/* Receives the SNTP callback and records that a sync event occurred. */
static void time_service_on_sync(struct timeval *time_of_day)
{
    struct tm local_time = {0};
    time_t synced_time = 0;

    if (time_of_day != NULL) {
        synced_time = time_of_day->tv_sec;
        localtime_r(&synced_time, &local_time);
        ESP_LOGI(TAG,
                 "SNTP sync completed: %04d-%02d-%02d %02d:%02d:%02d",
                 local_time.tm_year + 1900,
                 local_time.tm_mon + 1,
                 local_time.tm_mday,
                 local_time.tm_hour,
                 local_time.tm_min,
                 local_time.tm_sec);
    }

    s_sync_notification_pending = true;
}

/* Initializes timezone state and resets any earlier SNTP session. */
esp_err_t time_service_init(void)
{
    setenv("TZ", LOCAL_TIMEZONE, 1);
    tzset();

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    s_sync_notification_pending = false;

    ESP_LOGI(TAG, "Time service initialized with timezone %s", LOCAL_TIMEZONE);

    return ESP_OK;
}

/* Starts or restarts SNTP after Wi-Fi becomes available. */
esp_err_t time_service_start_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, SNTP_SERVER);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_time_sync_notification_cb(time_service_on_sync);

    if (esp_sntp_enabled()) {
        esp_sntp_restart();
        ESP_LOGI(TAG, "SNTP restarted with server %s", SNTP_SERVER);
    } else {
        esp_sntp_init();
        ESP_LOGI(TAG, "SNTP started with server %s", SNTP_SERVER);
    }

    return ESP_OK;
}

/* Reads the current local system time into the caller-provided structure. */
esp_err_t time_service_get_local_time(struct tm *timeinfo)
{
    time_t current_time = 0;

    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time(&current_time);
    if (localtime_r(&current_time, timeinfo) == NULL) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Writes a local time value into the system clock. */
esp_err_t time_service_set_local_time(const struct tm *timeinfo)
{
    struct tm normalized_time;
    struct timeval new_time = {0};
    time_t epoch_seconds = 0;

    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    normalized_time = *timeinfo;
    normalized_time.tm_isdst = -1;
    epoch_seconds = mktime(&normalized_time);
    if (epoch_seconds == (time_t)-1) {
        return ESP_ERR_INVALID_ARG;
    }

    new_time.tv_sec = epoch_seconds;
    if (settimeofday(&new_time, NULL) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Checks whether the system clock has reached a plausible synchronized date. */
bool time_service_has_valid_time(void)
{
    time_t current_time = 0;

    time(&current_time);

    return current_time >= VALID_TIME_THRESHOLD;
}

/* Returns and clears the one-shot SNTP completion notification. */
bool time_service_take_sync_notification(void)
{
    bool had_notification = s_sync_notification_pending;
    s_sync_notification_pending = false;
    return had_notification;
}
