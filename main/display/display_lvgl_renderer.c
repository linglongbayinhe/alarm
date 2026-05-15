#include "display_lvgl_renderer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "screens.h"
#include "status_lvgl_image.h"
#include "weather_lvgl_image.h"

#define DISPLAY_LVGL_RENDERER_REFRESH_MS     1000
#define DISPLAY_LVGL_RENDERER_TIME_BUF_SIZE  8
#define DISPLAY_LVGL_RENDERER_DATE_BUF_SIZE  16

static const char *const DISPLAY_LVGL_RENDERER_CHINESE_WEEKDAYS[7] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六",
};

/* Last displayed local time as minutes since midnight; -2 = showing placeholder, -1 = unknown. */
static int s_last_hm = -1;
/* Last calendar day shown on date_label; y == -2 means placeholder "----.--.--". */
static int s_date_y = -1;
static int s_date_m = -1;
static int s_date_d = -1;
static portMUX_TYPE s_view_model_lock = portMUX_INITIALIZER_UNLOCKED;
static display_view_model_t s_latest_view_model;
static bool s_latest_view_model_valid;
static bool s_weather_labels_cached;
static char s_last_weather_text[DISPLAY_WEATHER_CONDITION_TEXT_SIZE];
static char s_last_temperature_text[DISPLAY_WEATHER_TEMPERATURE_TEXT_SIZE];

static bool display_lvgl_renderer_copy_view_model(display_view_model_t *view_model)
{
    bool valid = false;

    if (view_model == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_view_model_lock);
    valid = s_latest_view_model_valid;
    if (valid) {
        *view_model = s_latest_view_model;
    }
    taskEXIT_CRITICAL(&s_view_model_lock);

    return valid;
}

static void display_lvgl_renderer_update_weather_labels(const display_weather_panel_t *panel)
{
    const char *weather_text = "";
    const char *temperature_text = "";

    if ((panel != NULL) && panel->visible) {
        weather_text = panel->condition_text;
        temperature_text = panel->temperature_text;
    }

    if (!s_weather_labels_cached ||
        (strcmp(s_last_weather_text, weather_text) != 0)) {
        if (objects.weather_label != NULL) {
            lv_label_set_text(objects.weather_label, weather_text);
        }
        snprintf(s_last_weather_text, sizeof(s_last_weather_text), "%s", weather_text);
    }

    if (!s_weather_labels_cached ||
        (strcmp(s_last_temperature_text, temperature_text) != 0)) {
        if (objects.temprature_label != NULL) {
            lv_label_set_text(objects.temprature_label, temperature_text);
        }
        snprintf(s_last_temperature_text, sizeof(s_last_temperature_text), "%s", temperature_text);
    }

    s_weather_labels_cached = true;
}

static void display_lvgl_renderer_time_cb(lv_timer_t *timer)
{
    display_view_model_t view_model = {0};
    struct tm now = {0};
    char tbuf[DISPLAY_LVGL_RENDERER_TIME_BUF_SIZE];
    char dbuf[DISPLAY_LVGL_RENDERER_DATE_BUF_SIZE];

    (void)timer;
    (void)display_lvgl_renderer_copy_view_model(&view_model);

    if (objects.weather_image != NULL) {
        weather_lvgl_image_update(objects.weather_image, &view_model.weather_panel);
        display_lvgl_renderer_update_weather_labels(&view_model.weather_panel);
    }

    if (objects.wifi_image != NULL) {
        status_lvgl_image_update(objects.wifi_image, &view_model.top_right_icon);
    }

    if (!view_model.time_valid) {
        if (objects.time_label != NULL && s_last_hm != -2) {
            lv_label_set_text(objects.time_label, "--:--");
            s_last_hm = -2;
        }
        if (s_date_y != -2) {
            if (objects.date_label != NULL) {
                lv_label_set_text(objects.date_label, "----.--.--");
            }
            if (objects.week_label != NULL) {
                lv_label_set_text(objects.week_label, "");
            }
            s_date_y = -2;
            s_date_m = -1;
            s_date_d = -1;
        }
        return;
    }

    now = view_model.current_time;

    if (objects.time_label != NULL) {
        int hm = now.tm_hour * 60 + now.tm_min;
        if (hm != s_last_hm) {
            s_last_hm = hm;
            strftime(tbuf, sizeof(tbuf), "%H:%M", &now);
            lv_label_set_text(objects.time_label, tbuf);
        }
    }

    if ((objects.date_label != NULL) || (objects.week_label != NULL)) {
        int calendar_changed = (s_date_y < 0) || (s_date_y != now.tm_year) ||
                               (s_date_m != now.tm_mon) || (s_date_d != now.tm_mday);
        if (calendar_changed) {
            s_date_y = now.tm_year;
            s_date_m = now.tm_mon;
            s_date_d = now.tm_mday;
            if (objects.date_label != NULL) {
                strftime(dbuf, sizeof(dbuf), "%Y.%m.%d", &now);
                lv_label_set_text(objects.date_label, dbuf);
            }
            if (objects.week_label != NULL) {
                lv_label_set_text(objects.week_label, DISPLAY_LVGL_RENDERER_CHINESE_WEEKDAYS[now.tm_wday]);
            }
        }
    }
}

void display_lvgl_renderer_init(void)
{
    lv_timer_create(display_lvgl_renderer_time_cb, DISPLAY_LVGL_RENDERER_REFRESH_MS, NULL);
}

void display_lvgl_renderer_set_view_model(const display_view_model_t *view_model)
{
    if (view_model == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_view_model_lock);
    s_latest_view_model = *view_model;
    s_latest_view_model_valid = true;
    taskEXIT_CRITICAL(&s_view_model_lock);
}
