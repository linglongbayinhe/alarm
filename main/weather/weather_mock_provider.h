#ifndef WEATHER_MOCK_PROVIDER_H
#define WEATHER_MOCK_PROVIDER_H

#include "esp_err.h"
#include "weather_types.h"

typedef enum {
    WEATHER_MOCK_SCENARIO_CLEAR_DAY = 0,
    WEATHER_MOCK_SCENARIO_CLEAR_NIGHT = 1,
    WEATHER_MOCK_SCENARIO_CLOUDY = 2,
    WEATHER_MOCK_SCENARIO_RAIN = 3,
    WEATHER_MOCK_SCENARIO_THUNDER = 4,
    WEATHER_MOCK_SCENARIO_STALE_CACHE = 5,
    WEATHER_MOCK_SCENARIO_LOADING = 6,
    WEATHER_MOCK_SCENARIO_ERROR = 7,
} weather_mock_scenario_t;

void weather_mock_provider_set_scenario(weather_mock_scenario_t scenario);
weather_mock_scenario_t weather_mock_provider_get_scenario(void);
esp_err_t weather_mock_provider_get_snapshot(weather_snapshot_t *snapshot);

#endif
