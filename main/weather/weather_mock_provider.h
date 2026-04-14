#ifndef WEATHER_MOCK_PROVIDER_H
#define WEATHER_MOCK_PROVIDER_H

#include "esp_err.h"
#include "weather_types.h"

typedef enum {
    WEATHER_MOCK_UNKNOWN = 0,
    WEATHER_MOCK_CLEAR_DAY = 1,
    WEATHER_MOCK_CLEAR_NIGHT = 2,
    WEATHER_MOCK_CLOUDY_DAY = 3,
    WEATHER_MOCK_CLOUDY_NIGHT = 4,
    WEATHER_MOCK_OVERCAST = 5,
    WEATHER_MOCK_LIGHT_RAIN = 6,
    WEATHER_MOCK_MODERATE_RAIN = 7,
    WEATHER_MOCK_HEAVY_RAIN = 8,
    WEATHER_MOCK_SHOWER = 9,
    WEATHER_MOCK_THUNDERSTORM = 10,
    WEATHER_MOCK_SNOW = 11,
    WEATHER_MOCK_FOG = 12,
    WEATHER_MOCK_HAZE = 13,
    WEATHER_MOCK_DUST_STORM = 14,
    WEATHER_MOCK_WINDY = 15,
} weather_mock_scenario_t;

void weather_mock_provider_set_scenario(weather_mock_scenario_t scenario);
weather_mock_scenario_t weather_mock_provider_get_scenario(void);
esp_err_t weather_mock_provider_get_snapshot(weather_snapshot_t *snapshot);

#endif
