#ifndef RTC_SERVICE_H
#define RTC_SERVICE_H

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

esp_err_t rtc_service_init(void);
esp_err_t rtc_service_read(struct tm *timeinfo);
esp_err_t rtc_service_write(const struct tm *timeinfo);
bool rtc_service_has_valid_time(void);
bool rtc_service_is_ready(void);

#endif
