#ifndef STATUS_PRESENTER_H
#define STATUS_PRESENTER_H

#include <stdbool.h>
#include <time.h>

#include "display_service.h"
#include "esp_err.h"
#include "weather_types.h"

typedef struct {
    bool wifi_connected;
    bool wifi_rssi_valid;
    int wifi_rssi;
    bool time_valid;
    struct tm current_time;
    bool weather_snapshot_valid;
    weather_snapshot_t weather_snapshot;
} status_presenter_input_t;

esp_err_t status_presenter_build_display_model(const status_presenter_input_t *input,
                                               display_view_model_t *output);

#endif
