#ifndef WEATHER_PRESENTER_H
#define WEATHER_PRESENTER_H

#include "display_view_model.h"
#include "esp_err.h"
#include "weather_types.h"

esp_err_t weather_presenter_build_panel_model(const weather_snapshot_t *snapshot,
                                              display_weather_panel_t *output);

#endif
