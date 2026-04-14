#ifndef DISPLAY_VIEW_MODEL_H
#define DISPLAY_VIEW_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "display_weather_icon_types.h"

typedef enum {
    DISPLAY_STATUS_ICON_KIND_NONE = 0,
    DISPLAY_STATUS_ICON_KIND_WIFI = 1,
} display_wifi_status_icon_type;

typedef enum {
    DISPLAY_STATUS_ICON_VARIANT_NORMAL = 0,
    DISPLAY_STATUS_ICON_VARIANT_ALERT = 1,
} display_wifi_status_icon_variant_t;

typedef struct {
    bool visible;
    display_wifi_status_icon_type kind;
    display_wifi_status_icon_variant_t variant;
    uint8_t level;
} display_wifi_status_icon_t;

#define DISPLAY_WEATHER_TEMPERATURE_TEXT_SIZE 8
#define DISPLAY_WEATHER_CONDITION_TEXT_SIZE  16
#define DISPLAY_WEATHER_DETAILS_TEXT_SIZE    24
#define DISPLAY_WEATHER_FOOTER_TEXT_SIZE     24

typedef struct display_weather_panel {
    bool visible;
    bool stale;
    bool show_condition_text;
    weather_icon_kind_t icon;
    char temperature_text[DISPLAY_WEATHER_TEMPERATURE_TEXT_SIZE];
    char condition_text[DISPLAY_WEATHER_CONDITION_TEXT_SIZE];
    char details_text[DISPLAY_WEATHER_DETAILS_TEXT_SIZE];
    char footer_text[DISPLAY_WEATHER_FOOTER_TEXT_SIZE];
} display_weather_panel_t;

typedef struct {
    display_wifi_status_icon_t top_right_icon;
    display_weather_panel_t weather_panel;
    bool time_valid;
    struct tm current_time;
} display_view_model_t;

#endif
