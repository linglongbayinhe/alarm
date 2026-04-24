#ifndef WEATHER_DEVICE_SERVICE_PROVIDER_H
#define WEATHER_DEVICE_SERVICE_PROVIDER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "weather_types.h"

esp_err_t weather_device_service_provider_start(EventGroupHandle_t connected_event_group,
                                                EventBits_t connected_bit);
esp_err_t weather_device_service_provider_get_snapshot(weather_snapshot_t *snapshot);
void weather_device_service_provider_request_refresh(void);

#endif
