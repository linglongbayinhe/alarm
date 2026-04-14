#include "weather_mock_provider.h"

#include <string.h>
#include <time.h>

static weather_mock_scenario_t s_current_scenario = WEATHER_MOCK_CLEAR_DAY;

static void weather_mock_provider_fill_ready_snapshot(weather_snapshot_t *snapshot,
                                                      weather_condition_t condition,
                                                      bool is_daytime,
                                                      int current_temperature_c,
                                                      int high_temperature_c,
                                                      int low_temperature_c,
                                                      uint8_t humidity_percent,
                                                      time_t updated_at_utc)
{
    snapshot->state = WEATHER_DATA_STATE_READY;
    snapshot->condition = condition;
    snapshot->is_daytime = is_daytime;
    snapshot->has_current_temperature = true;
    snapshot->current_temperature_c = (int16_t)current_temperature_c;
    snapshot->has_daily_range = true;
    snapshot->high_temperature_c = (int16_t)high_temperature_c;
    snapshot->low_temperature_c = (int16_t)low_temperature_c;
    snapshot->has_humidity = true;
    snapshot->humidity_percent = humidity_percent;
    snapshot->has_update_time = true;
    snapshot->updated_at_utc = updated_at_utc;
}

void weather_mock_provider_set_scenario(weather_mock_scenario_t scenario)
{
    s_current_scenario = scenario;
}

weather_mock_scenario_t weather_mock_provider_get_scenario(void)
{
    return s_current_scenario;
}

esp_err_t weather_mock_provider_get_snapshot(weather_snapshot_t *snapshot)
{
    time_t now = time(NULL);

    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    switch (s_current_scenario) {
        case WEATHER_MOCK_UNKNOWN:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_UNKNOWN,
                                                      true,
                                                      0,
                                                      0,
                                                      0,
                                                      0,
                                                      now);
            break;
        case WEATHER_MOCK_CLEAR_DAY:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_CLEAR_DAY,
                                                      true,
                                                      26,
                                                      29,
                                                      22,
                                                      45,
                                                      now);
            break;
        case WEATHER_MOCK_CLEAR_NIGHT:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_CLEAR_NIGHT,
                                                      false,
                                                      21,
                                                      25,
                                                      19,
                                                      60,
                                                      now);
            break;
        case WEATHER_MOCK_CLOUDY_DAY:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_CLOUDY_DAY,
                                                      true,
                                                      24,
                                                      27,
                                                      20,
                                                      63,
                                                      now);
            break;
        case WEATHER_MOCK_CLOUDY_NIGHT:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_CLOUDY_NIGHT,
                                                      false,
                                                      22,
                                                      25,
                                                      19,
                                                      68,
                                                      now);
            break;
        case WEATHER_MOCK_OVERCAST:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_OVERCAST,
                                                      true,
                                                      23,
                                                      25,
                                                      20,
                                                      72,
                                                      now);
            break;
        case WEATHER_MOCK_LIGHT_RAIN:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_LIGHT_RAIN,
                                                      true,
                                                      19,
                                                      22,
                                                      17,
                                                      84,
                                                      now);
            break;
        case WEATHER_MOCK_MODERATE_RAIN:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_MODERATE_RAIN,
                                                      true,
                                                      18,
                                                      20,
                                                      16,
                                                      88,
                                                      now);
            break;
        case WEATHER_MOCK_HEAVY_RAIN:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_HEAVY_RAIN,
                                                      true,
                                                      17,
                                                      19,
                                                      15,
                                                      92,
                                                      now);
            break;
        case WEATHER_MOCK_SHOWER:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_SHOWER,
                                                      true,
                                                      20,
                                                      23,
                                                      18,
                                                      86,
                                                      now);
            break;
        case WEATHER_MOCK_THUNDERSTORM:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_THUNDERSTORM,
                                                      true,
                                                      20,
                                                      23,
                                                      18,
                                                      91,
                                                      now);
            break;
        case WEATHER_MOCK_SNOW:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_SNOW,
                                                      true,
                                                      0,
                                                      2,
                                                      -3,
                                                      78,
                                                      now);
            break;
        case WEATHER_MOCK_FOG:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_FOG,
                                                      true,
                                                      16,
                                                      19,
                                                      14,
                                                      95,
                                                      now);
            break;
        case WEATHER_MOCK_HAZE:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_HAZE,
                                                      true,
                                                      25,
                                                      28,
                                                      21,
                                                      70,
                                                      now);
            break;
        case WEATHER_MOCK_DUST_STORM:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_DUST_STORM,
                                                      true,
                                                      28,
                                                      31,
                                                      24,
                                                      35,
                                                      now);
            break;
        case WEATHER_MOCK_WINDY:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_WINDY,
                                                      true,
                                                      22,
                                                      25,
                                                      18,
                                                      55,
                                                      now);
            break;
        default:
            snapshot->state = WEATHER_DATA_STATE_READY;
            snapshot->condition = WEATHER_CONDITION_UNKNOWN;
            snapshot->is_daytime = true;
            break;
    }

    return ESP_OK;
}
