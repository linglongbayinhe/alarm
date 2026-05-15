#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

esp_err_t time_service_init(void);
esp_err_t time_service_start_sntp(void);
esp_err_t time_service_get_local_time(struct tm *timeinfo);
esp_err_t time_service_set_local_time(const struct tm *timeinfo);
bool time_service_has_valid_time(void);
bool time_service_take_sync_notification(void);

#endif
