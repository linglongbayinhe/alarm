#include "weather_presenter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static void weather_presenter_copy_text(char *destination, size_t destination_size, const char *source)
{
    snprintf(destination, destination_size, "%s", source);
}

static display_weather_icon_kind_t weather_presenter_map_icon(const weather_snapshot_t *snapshot)
{
    switch (snapshot->condition) {
        case WEATHER_CONDITION_CLEAR:
            return snapshot->is_daytime ?
                   DISPLAY_WEATHER_ICON_KIND_CLEAR_DAY :
                   DISPLAY_WEATHER_ICON_KIND_CLEAR_NIGHT;
        case WEATHER_CONDITION_PARTLY_CLOUDY:
            return snapshot->is_daytime ?
                   DISPLAY_WEATHER_ICON_KIND_PARTLY_CLOUDY_DAY :
                   DISPLAY_WEATHER_ICON_KIND_PARTLY_CLOUDY_NIGHT;
        case WEATHER_CONDITION_CLOUDY:
            return DISPLAY_WEATHER_ICON_KIND_CLOUDY;
        case WEATHER_CONDITION_OVERCAST:
            return DISPLAY_WEATHER_ICON_KIND_OVERCAST;
        case WEATHER_CONDITION_LIGHT_RAIN:
            return DISPLAY_WEATHER_ICON_KIND_LIGHT_RAIN;
        case WEATHER_CONDITION_RAIN:
            return DISPLAY_WEATHER_ICON_KIND_RAIN;
        case WEATHER_CONDITION_THUNDERSTORM:
            return DISPLAY_WEATHER_ICON_KIND_THUNDERSTORM;
        case WEATHER_CONDITION_SNOW:
            return DISPLAY_WEATHER_ICON_KIND_SNOW;
        case WEATHER_CONDITION_FOG:
            return DISPLAY_WEATHER_ICON_KIND_FOG;
        case WEATHER_CONDITION_WINDY:
            return DISPLAY_WEATHER_ICON_KIND_WINDY;
        case WEATHER_CONDITION_UNKNOWN:
        default:
            return DISPLAY_WEATHER_ICON_KIND_UNKNOWN;
    }
}

static const char *weather_presenter_condition_text(weather_condition_t condition)
{
    switch (condition) {
        case WEATHER_CONDITION_CLEAR:
            return "CLEAR";
        case WEATHER_CONDITION_PARTLY_CLOUDY:
            return "PART CLOUD";
        case WEATHER_CONDITION_CLOUDY:
            return "CLOUDY";
        case WEATHER_CONDITION_OVERCAST:
            return "OVERCAST";
        case WEATHER_CONDITION_LIGHT_RAIN:
            return "LIGHT RAIN";
        case WEATHER_CONDITION_RAIN:
            return "RAIN";
        case WEATHER_CONDITION_THUNDERSTORM:
            return "STORM";
        case WEATHER_CONDITION_SNOW:
            return "SNOW";
        case WEATHER_CONDITION_FOG:
            return "FOG";
        case WEATHER_CONDITION_WINDY:
            return "WINDY";
        case WEATHER_CONDITION_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

static bool weather_presenter_should_show_condition_text(const weather_snapshot_t *snapshot)
{
    if ((snapshot->state == WEATHER_DATA_STATE_LOADING) ||
        (snapshot->state == WEATHER_DATA_STATE_ERROR)) {
        return true;
    }

    switch (snapshot->condition) {
        case WEATHER_CONDITION_LIGHT_RAIN:
        case WEATHER_CONDITION_RAIN:
        case WEATHER_CONDITION_THUNDERSTORM:
        case WEATHER_CONDITION_SNOW:
        case WEATHER_CONDITION_FOG:
        case WEATHER_CONDITION_WINDY:
        case WEATHER_CONDITION_UNKNOWN:
            return true;
        case WEATHER_CONDITION_CLEAR:
        case WEATHER_CONDITION_PARTLY_CLOUDY:
        case WEATHER_CONDITION_CLOUDY:
        case WEATHER_CONDITION_OVERCAST:
        default:
            return false;
    }
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

static void weather_presenter_format_details(const weather_snapshot_t *snapshot,
                                             char *buffer,
                                             size_t buffer_size)
{
    char range_text[16] = {0};
    char humidity_text[16] = {0};

    if (snapshot->has_daily_range) {
        snprintf(range_text,
                 sizeof(range_text),
                 "H%d L%d",
                 (int)snapshot->high_temperature_c,
                 (int)snapshot->low_temperature_c);
    }

    if (snapshot->has_humidity) {
        snprintf(humidity_text,
                 sizeof(humidity_text),
                 "HUM %u%%",
                 (unsigned int)snapshot->humidity_percent);
    }

    if ((range_text[0] != '\0') && (humidity_text[0] != '\0')) {
        snprintf(buffer, buffer_size, "%s %s", range_text, humidity_text);
    } else if (range_text[0] != '\0') {
        weather_presenter_copy_text(buffer, buffer_size, range_text);
    } else if (humidity_text[0] != '\0') {
        weather_presenter_copy_text(buffer, buffer_size, humidity_text);
    } else {
        buffer[0] = '\0';
    }
}

static void weather_presenter_format_footer(const weather_snapshot_t *snapshot,
                                            char *buffer,
                                            size_t buffer_size)
{
    struct tm local_time = {0};

    switch (snapshot->state) {
        case WEATHER_DATA_STATE_LOADING:
            weather_presenter_copy_text(buffer, buffer_size, "WAITING DATA");
            return;
        case WEATHER_DATA_STATE_ERROR:
            weather_presenter_copy_text(buffer, buffer_size, "RETRY PENDING");
            return;
        case WEATHER_DATA_STATE_STALE:
            if (!snapshot->has_update_time) {
                weather_presenter_copy_text(buffer, buffer_size, "OFFLINE CACHE");
                return;
            }
            if (localtime_r(&snapshot->updated_at_utc, &local_time) == NULL) {
                weather_presenter_copy_text(buffer, buffer_size, "OFFLINE CACHE");
                return;
            }
            snprintf(buffer,
                     buffer_size,
                     "STALE %02d:%02d",
                     local_time.tm_hour,
                     local_time.tm_min);
            return;
        case WEATHER_DATA_STATE_READY:
            if (!snapshot->has_update_time) {
                buffer[0] = '\0';
                return;
            }
            if (localtime_r(&snapshot->updated_at_utc, &local_time) == NULL) {
                buffer[0] = '\0';
                return;
            }
            snprintf(buffer,
                     buffer_size,
                     "UPDATED %02d:%02d",
                     local_time.tm_hour,
                     local_time.tm_min);
            return;
        case WEATHER_DATA_STATE_EMPTY:
        default:
            buffer[0] = '\0';
            return;
    }
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
    output->stale = (snapshot->state == WEATHER_DATA_STATE_STALE);
    output->icon = weather_presenter_map_icon(snapshot);
    output->show_condition_text = weather_presenter_should_show_condition_text(snapshot);

    if (snapshot->state == WEATHER_DATA_STATE_LOADING) {
        output->icon = DISPLAY_WEATHER_ICON_KIND_UNKNOWN;
        weather_presenter_copy_text(output->temperature_text,
                                    sizeof(output->temperature_text),
                                    "--");
        weather_presenter_copy_text(output->condition_text,
                                    sizeof(output->condition_text),
                                    "LOADING");
        weather_presenter_copy_text(output->footer_text,
                                    sizeof(output->footer_text),
                                    "WAITING DATA");
        return ESP_OK;
    }

    if (snapshot->state == WEATHER_DATA_STATE_ERROR) {
        output->icon = DISPLAY_WEATHER_ICON_KIND_UNKNOWN;
        weather_presenter_copy_text(output->temperature_text,
                                    sizeof(output->temperature_text),
                                    "--");
        weather_presenter_copy_text(output->condition_text,
                                    sizeof(output->condition_text),
                                    "NO DATA");
        weather_presenter_copy_text(output->footer_text,
                                    sizeof(output->footer_text),
                                    "RETRY PENDING");
        return ESP_OK;
    }

    weather_presenter_format_temperature(snapshot,
                                         output->temperature_text,
                                         sizeof(output->temperature_text));
    weather_presenter_copy_text(output->condition_text,
                                sizeof(output->condition_text),
                                weather_presenter_condition_text(snapshot->condition));
    weather_presenter_format_details(snapshot,
                                     output->details_text,
                                     sizeof(output->details_text));
    weather_presenter_format_footer(snapshot,
                                    output->footer_text,
                                    sizeof(output->footer_text));

    return ESP_OK;
}
