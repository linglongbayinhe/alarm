#ifndef WEATHER_TYPES_H
#define WEATHER_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

typedef enum {
    WEATHER_DATA_STATE_EMPTY = 0,
    WEATHER_DATA_STATE_LOADING = 1,
    WEATHER_DATA_STATE_READY = 2,
    WEATHER_DATA_STATE_STALE = 3,
    WEATHER_DATA_STATE_ERROR = 4,
} weather_data_state_t;

typedef enum {
    WEATHER_CONDITION_UNKNOWN = 0,
    WEATHER_CONDITION_CLEAR = 1,
    WEATHER_CONDITION_PARTLY_CLOUDY = 2,
    WEATHER_CONDITION_CLOUDY = 3,
    WEATHER_CONDITION_OVERCAST = 4,
    WEATHER_CONDITION_LIGHT_RAIN = 5,
    WEATHER_CONDITION_RAIN = 6,
    WEATHER_CONDITION_THUNDERSTORM = 7,
    WEATHER_CONDITION_SNOW = 8,
    WEATHER_CONDITION_FOG = 9,
    WEATHER_CONDITION_WINDY = 10,
} weather_condition_t;

typedef struct {
    weather_data_state_t state;
    weather_condition_t condition;
    bool is_daytime;
    bool has_current_temperature;
    int16_t current_temperature_c;
    bool has_daily_range;
    int16_t high_temperature_c;
    int16_t low_temperature_c;
    bool has_humidity;
    uint8_t humidity_percent;
    bool has_update_time;
    time_t updated_at_utc;
} weather_snapshot_t;

#endif
