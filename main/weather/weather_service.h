#ifndef WEATHER_SERVICE_H
#define WEATHER_SERVICE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "weather_types.h"

esp_err_t weather_service_prepare(void);
esp_err_t weather_service_start(EventGroupHandle_t connected_event_group,
                                EventBits_t connected_bit);
esp_err_t weather_service_get_snapshot(weather_snapshot_t *snapshot);
void weather_service_request_refresh(void);

#endif
