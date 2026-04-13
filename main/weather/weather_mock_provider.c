#include "weather_mock_provider.h"

#include <string.h>
#include <time.h>

static weather_mock_scenario_t s_current_scenario = WEATHER_MOCK_SCENARIO_CLEAR_DAY;

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
        case WEATHER_MOCK_SCENARIO_CLEAR_DAY:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_CLEAR,
                                                      true,
                                                      26,
                                                      29,
                                                      22,
                                                      45,
                                                      now);
            break;
        case WEATHER_MOCK_SCENARIO_CLEAR_NIGHT:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_CLEAR,
                                                      false,
                                                      21,
                                                      25,
                                                      19,
                                                      60,
                                                      now);
            break;
        case WEATHER_MOCK_SCENARIO_CLOUDY:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_CLOUDY,
                                                      true,
                                                      24,
                                                      27,
                                                      20,
                                                      63,
                                                      now);
            break;
        case WEATHER_MOCK_SCENARIO_RAIN:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_RAIN,
                                                      true,
                                                      18,
                                                      20,
                                                      16,
                                                      88,
                                                      now);
            break;
        case WEATHER_MOCK_SCENARIO_THUNDER:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_THUNDERSTORM,
                                                      true,
                                                      20,
                                                      23,
                                                      18,
                                                      91,
                                                      now);
            break;
        case WEATHER_MOCK_SCENARIO_STALE_CACHE:
            weather_mock_provider_fill_ready_snapshot(snapshot,
                                                      WEATHER_CONDITION_LIGHT_RAIN,
                                                      true,
                                                      19,
                                                      22,
                                                      17,
                                                      84,
                                                      now - (2 * 60 * 60));
            snapshot->state = WEATHER_DATA_STATE_STALE;
            break;
        case WEATHER_MOCK_SCENARIO_LOADING:
            snapshot->state = WEATHER_DATA_STATE_LOADING;
            snapshot->condition = WEATHER_CONDITION_UNKNOWN;
            snapshot->is_daytime = true;
            break;
        case WEATHER_MOCK_SCENARIO_ERROR:
            snapshot->state = WEATHER_DATA_STATE_ERROR;
            snapshot->condition = WEATHER_CONDITION_UNKNOWN;
            snapshot->is_daytime = true;
            break;
        default:
            snapshot->state = WEATHER_DATA_STATE_EMPTY;
            snapshot->condition = WEATHER_CONDITION_UNKNOWN;
            break;
    }

    return ESP_OK;
}
