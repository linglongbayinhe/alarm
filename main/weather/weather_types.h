#ifndef WEATHER_TYPES_H
#define WEATHER_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define WEATHER_TEXT_SIZE      64
#define WEATHER_ICON_TEXT_SIZE 24

typedef enum {
    WEATHER_DATA_STATE_EMPTY = 0,
    WEATHER_DATA_STATE_LOADING = 1,
    WEATHER_DATA_STATE_READY = 2,
    WEATHER_DATA_STATE_STALE = 3,
    WEATHER_DATA_STATE_ERROR = 4,
} weather_data_state_t;

typedef enum {
    WEATHER_CONDITION_UNKNOWN = 0,
    WEATHER_CONDITION_CLEAR_DAY = 1,
    WEATHER_CONDITION_CLEAR_NIGHT = 2,
    WEATHER_CONDITION_CLOUDY_DAY = 3,
    WEATHER_CONDITION_CLOUDY_NIGHT = 4,
    WEATHER_CONDITION_OVERCAST = 5,
    WEATHER_CONDITION_LIGHT_RAIN = 6,
    WEATHER_CONDITION_MODERATE_RAIN = 7,
    WEATHER_CONDITION_HEAVY_RAIN = 8,
    WEATHER_CONDITION_SHOWER = 9,
    WEATHER_CONDITION_THUNDERSTORM = 10,
    WEATHER_CONDITION_SNOW = 11,
    WEATHER_CONDITION_FOG = 12,
    WEATHER_CONDITION_HAZE = 13,
    WEATHER_CONDITION_DUST_STORM = 14,
    WEATHER_CONDITION_WINDY = 15,
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
    bool has_weather_text;
    char weather_text[WEATHER_TEXT_SIZE];
    bool has_weather_icon_text;
    char weather_icon_text[WEATHER_ICON_TEXT_SIZE];
    bool has_target_volume;
    int16_t target_volume;
    bool has_current_volume;
    int16_t current_volume;
    bool has_volume_level;
    int16_t volume_level;
} weather_snapshot_t;

#endif
