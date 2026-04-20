#include "ui_bridge.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "screens.h"
#include "time_service.h"
#include "weather_lvgl_image.h"

#define UI_BRIDGE_REFRESH_MS     1000
#define UI_BRIDGE_TIME_BUF_SIZE  8
#define UI_BRIDGE_DATE_BUF_SIZE  16

static const char *const UI_BRIDGE_CHINESE_WEEKDAYS[7] = {
    "周日", "周一", "周二", "周三", "周四", "周五", "周六",
};

/* Last displayed local time as minutes since midnight; -2 = showing placeholder. */
static int s_last_hm = -2;
/* Last calendar day shown on date_label; y == -2 means placeholder "----.--.--". */
static int s_date_y = -1;
static int s_date_m = -1;
static int s_date_d = -1;
static portMUX_TYPE s_weather_panel_lock = portMUX_INITIALIZER_UNLOCKED;
static display_weather_panel_t s_latest_weather_panel;
static bool s_latest_weather_panel_valid;

static bool ui_bridge_copy_weather_panel(display_weather_panel_t *panel)
{
    bool valid = false;

    if (panel == NULL) {
        return false;
    }

    taskENTER_CRITICAL(&s_weather_panel_lock);
    valid = s_latest_weather_panel_valid;
    if (valid) {
        *panel = s_latest_weather_panel;
    }
    taskEXIT_CRITICAL(&s_weather_panel_lock);

    return valid;
}

static void ui_bridge_time_cb(lv_timer_t *timer)
{
    struct tm now;
    char tbuf[UI_BRIDGE_TIME_BUF_SIZE];
    char dbuf[UI_BRIDGE_DATE_BUF_SIZE];
    display_weather_panel_t weather_panel;

    (void)timer;

    if (objects.weather_image != NULL) {
        memset(&weather_panel, 0, sizeof(weather_panel));
        (void)ui_bridge_copy_weather_panel(&weather_panel);
        weather_lvgl_image_update(objects.weather_image, &weather_panel);
    }

    if (!time_service_has_valid_time()) {
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

    if (time_service_get_local_time(&now) != ESP_OK) {
        return;
    }

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
                lv_label_set_text(objects.week_label, UI_BRIDGE_CHINESE_WEEKDAYS[now.tm_wday]);
            }
        }
    }
}

void ui_bridge_init(void)
{
    lv_timer_create(ui_bridge_time_cb, UI_BRIDGE_REFRESH_MS, NULL);
}

void ui_bridge_set_view_model(const display_view_model_t *view_model)
{
    if (view_model == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_weather_panel_lock);
    s_latest_weather_panel = view_model->weather_panel;
    s_latest_weather_panel_valid = true;
    taskEXIT_CRITICAL(&s_weather_panel_lock);
}
