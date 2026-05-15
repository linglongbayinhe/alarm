#include "weather_presenter.h"

#include <stdio.h>
#include <string.h>

static void weather_presenter_copy_text(char *destination, size_t destination_size, const char *source)
{
    snprintf(destination, destination_size, "%s", source);
}

static weather_icon_kind_t weather_presenter_map_icon(const weather_snapshot_t *snapshot)
{
    if ((snapshot->condition < WEATHER_CONDITION_UNKNOWN) ||
        (snapshot->condition > WEATHER_CONDITION_WINDY)) {
        return WEATHER_ICON_UNKNOWN;
    }

    return (weather_icon_kind_t)snapshot->condition;
}

static const char *weather_presenter_condition_fallback_text(weather_condition_t condition)
{
    switch (condition) {
        case WEATHER_CONDITION_CLEAR_DAY:
            return "CLEAR DAY";
        case WEATHER_CONDITION_CLEAR_NIGHT:
            return "CLEAR NIGHT";
        case WEATHER_CONDITION_CLOUDY_DAY:
            return "CLOUDY DAY";
        case WEATHER_CONDITION_CLOUDY_NIGHT:
            return "CLOUDY NIGHT";
        case WEATHER_CONDITION_OVERCAST:
            return "OVERCAST";
        case WEATHER_CONDITION_LIGHT_RAIN:
            return "LIGHT RAIN";
        case WEATHER_CONDITION_MODERATE_RAIN:
            return "MODERATE RAIN";
        case WEATHER_CONDITION_HEAVY_RAIN:
            return "HEAVY RAIN";
        case WEATHER_CONDITION_SHOWER:
            return "SHOWER";
        case WEATHER_CONDITION_THUNDERSTORM:
            return "THUNDERSTORM";
        case WEATHER_CONDITION_SNOW:
            return "SNOW";
        case WEATHER_CONDITION_FOG:
            return "FOG";
        case WEATHER_CONDITION_HAZE:
            return "HAZE";
        case WEATHER_CONDITION_DUST_STORM:
            return "DUST STORM";
        case WEATHER_CONDITION_WINDY:
            return "WINDY";
        case WEATHER_CONDITION_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

static const char *weather_presenter_condition_text(const weather_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return "UNKNOWN";
    }

    if (snapshot->has_weather_text &&
        (snapshot->weather_text[0] != '\0')) {
        return snapshot->weather_text;
    }

    return weather_presenter_condition_fallback_text(snapshot->condition);
}

static void weather_presenter_format_temperature(const weather_snapshot_t *snapshot,
                                                 char *buffer,
                                                 size_t buffer_size)
{
    if (!snapshot->has_current_temperature) {
        weather_presenter_copy_text(buffer, buffer_size, "--");
        return;
    }

    snprintf(buffer, buffer_size, "%dC", (int)snapshot->current_temperature_c);
}

esp_err_t weather_presenter_build_panel_model(const weather_snapshot_t *snapshot,
                                              display_weather_panel_t *output)
{
    if ((snapshot == NULL) || (output == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(output, 0, sizeof(*output));

    if (snapshot->state == WEATHER_DATA_STATE_EMPTY) {
        return ESP_OK;
    }

    output->visible = true;
    output->icon = weather_presenter_map_icon(snapshot);

    if (snapshot->state == WEATHER_DATA_STATE_LOADING) {
        output->icon = WEATHER_ICON_UNKNOWN;
        weather_presenter_copy_text(output->temperature_text,
                                    sizeof(output->temperature_text),
                                    "--");
        weather_presenter_copy_text(output->condition_text,
                                    sizeof(output->condition_text),
                                    "LOADING");
        return ESP_OK;
    }

    if (snapshot->state == WEATHER_DATA_STATE_ERROR) {
        output->icon = WEATHER_ICON_UNKNOWN;
        weather_presenter_copy_text(output->temperature_text,
                                    sizeof(output->temperature_text),
                                    "--");
        weather_presenter_copy_text(output->condition_text,
                                    sizeof(output->condition_text),
                                    "NO DATA");
        return ESP_OK;
    }

    weather_presenter_format_temperature(snapshot,
                                         output->temperature_text,
                                         sizeof(output->temperature_text));
    weather_presenter_copy_text(output->condition_text,
                                sizeof(output->condition_text),
                                weather_presenter_condition_text(snapshot));

    return ESP_OK;
}
