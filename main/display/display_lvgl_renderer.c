#include "display_lvgl_renderer.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/libs/qrcode/lv_qrcode.h"
#include "src/libs/qrcode/qrcodegen.h"
#include "display_config.h"
#include "expression_lvgl.h"
#include "fonts.h"
#include "screens.h"
#include "status_lvgl_image.h"
#include "weather_lvgl_image.h"

#define DISPLAY_LVGL_RENDERER_REFRESH_MS     1000
#define DISPLAY_LVGL_RENDERER_TIME_BUF_SIZE  8
#define DISPLAY_LVGL_RENDERER_DATE_BUF_SIZE  16
#define DISPLAY_LVGL_RENDERER_QR_SIZE        132
#define DISPLAY_LVGL_RENDERER_QR_CACHE_SIZE  DISPLAY_PROVISIONING_QR_PAYLOAD_SIZE
#define DISPLAY_WEATHER_LABEL_GAP_PX         8
#define DISPLAY_WEATHER_RIGHT_MARGIN_PX      4

#define DISPLAY_TEXT_PROVISION_HINT       "\xE8\xAF\xB7\xE7\x94\xA8\xE5\xB0\x8F\xE7\xA8\x8B\xE5\xBA\x8F\xE6\x89\xAB\xE7\xA0\x81\xE9\x85\x8D\xE7\xBD\x91"
#define DISPLAY_TEXT_PROVISION_CONNECTING "\xE8\xBF\x9E\xE6\x8E\xA5\xE4\xB8\xAD\x2E\x2E\x2E"
#define DISPLAY_TEXT_PROVISION_SUCCESS    "\xE8\xBF\x9E\xE6\x8E\xA5\xE6\x88\x90\xE5\x8A\x9F"
#define DISPLAY_TEXT_PROVISION_FAILED     "\xE8\xBF\x9E\xE6\x8E\xA5\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x8C\xE8\xAF\xB7\xE9\x87\x8D\xE6\x96\xB0\xE6\x89\xAB\xE7\xA0\x81"

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
static lv_obj_t *s_provision_container;
static lv_obj_t *s_provision_qr;
static lv_obj_t *s_provision_wifi_icon;
static lv_obj_t *s_provision_message;
static display_provisioning_state_t s_last_provisioning_state = DISPLAY_PROVISIONING_STATE_HIDDEN;
static char s_last_qr_payload[DISPLAY_LVGL_RENDERER_QR_CACHE_SIZE];
static display_page_t s_last_page = DISPLAY_PAGE_STATUS;
static display_expression_kind_t s_last_expression = DISPLAY_EXPRESSION_IDLE;

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

static void display_lvgl_renderer_layout_temperature_label(void)
{
    int32_t weather_x;
    int32_t weather_w;
    int32_t temp_x;
    int32_t temp_w;

    if ((objects.weather_label == NULL) || (objects.temprature_label == NULL)) {
        return;
    }

    lv_obj_update_layout(objects.weather_label);
    lv_obj_update_layout(objects.temprature_label);

    weather_x = lv_obj_get_x(objects.weather_label);
    weather_w = lv_obj_get_width(objects.weather_label);
    temp_w = lv_obj_get_width(objects.temprature_label);
    temp_x = weather_x + weather_w + DISPLAY_WEATHER_LABEL_GAP_PX;

    if ((temp_x + temp_w) > (DISPLAY_WIDTH - DISPLAY_WEATHER_RIGHT_MARGIN_PX)) {
        temp_x = DISPLAY_WIDTH - DISPLAY_WEATHER_RIGHT_MARGIN_PX - temp_w;
        if (temp_x < weather_x) {
            temp_x = weather_x;
        }
    }

    lv_obj_set_x(objects.temprature_label, temp_x);
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

    if ((panel != NULL) && panel->visible) {
        display_lvgl_renderer_layout_temperature_label();
    }

    s_weather_labels_cached = true;
}

static void display_lvgl_renderer_set_status_visible(bool visible)
{
    lv_obj_t *status_objects[] = {
        objects.date_label,
        objects.week_label,
        objects.time_label,
        objects.weather_image,
        objects.weather_label,
        objects.temprature_label,
        objects.wifi_image,
    };
    size_t index = 0;

    for (index = 0; index < (sizeof(status_objects) / sizeof(status_objects[0])); ++index) {
        if (status_objects[index] == NULL) {
            continue;
        }
        if (visible) {
            lv_obj_remove_flag(status_objects[index], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(status_objects[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void display_lvgl_renderer_update_page(const display_view_model_t *view_model)
{
    display_page_t page = DISPLAY_PAGE_STATUS;
    display_expression_kind_t expression = DISPLAY_EXPRESSION_IDLE;

    if (view_model != NULL) {
        page = view_model->page;
        expression = view_model->expression;
    }
    if (page > DISPLAY_PAGE_EXPRESSION) {
        page = DISPLAY_PAGE_STATUS;
    }
    if (expression > DISPLAY_EXPRESSION_SAD) {
        expression = DISPLAY_EXPRESSION_IDLE;
    }

    if (expression != s_last_expression) {
        expression_lvgl_set_mode(expression);
        s_last_expression = expression;
    }
    if (page != s_last_page) {
        display_lvgl_renderer_set_status_visible(page == DISPLAY_PAGE_STATUS);
        expression_lvgl_set_visible(page == DISPLAY_PAGE_EXPRESSION);
        s_last_page = page;
    }
}

static const char *display_lvgl_renderer_provisioning_message(display_provisioning_state_t state)
{
    switch (state) {
    case DISPLAY_PROVISIONING_STATE_CONNECTING:
        return DISPLAY_TEXT_PROVISION_CONNECTING;
    case DISPLAY_PROVISIONING_STATE_SUCCESS:
        return DISPLAY_TEXT_PROVISION_SUCCESS;
    case DISPLAY_PROVISIONING_STATE_FAILED:
        return DISPLAY_TEXT_PROVISION_FAILED;
    case DISPLAY_PROVISIONING_STATE_QR:
        return DISPLAY_TEXT_PROVISION_HINT;
    case DISPLAY_PROVISIONING_STATE_HIDDEN:
    default:
        return "";
    }
}

static lv_result_t display_lvgl_renderer_qrcode_update_low_ecc(lv_obj_t *obj,
                                                               const void *data,
                                                               uint32_t data_len)
{
    lv_draw_buf_t *draw_buf = NULL;
    uint8_t *qr0 = NULL;
    uint8_t *data_tmp = NULL;
    int32_t qr_version = 0;
    int32_t qr_size = 0;
    int32_t scale = 0;

    if ((obj == NULL) || (data == NULL)) {
        return LV_RESULT_INVALID;
    }

    draw_buf = lv_canvas_get_draw_buf(obj);
    if (draw_buf == NULL) {
        return LV_RESULT_INVALID;
    }
    if (data_len > qrcodegen_BUFFER_LEN_MAX) {
        return LV_RESULT_INVALID;
    }

    lv_draw_buf_clear(draw_buf, NULL);
    lv_canvas_set_palette(obj, 0, lv_color_to_32(lv_color_white(), LV_OPA_COVER));
    lv_canvas_set_palette(obj, 1, lv_color_to_32(lv_color_black(), LV_OPA_COVER));
    lv_image_cache_drop(draw_buf);
    lv_obj_invalidate(obj);

    qr_version = qrcodegen_getMinFitVersion(qrcodegen_Ecc_LOW, data_len);
    if (qr_version <= 0) {
        return LV_RESULT_INVALID;
    }
    qr_size = qrcodegen_version2size(qr_version);
    if (qr_size <= 0) {
        return LV_RESULT_INVALID;
    }
    scale = draw_buf->header.w / qr_size;
    if (scale <= 0) {
        return LV_RESULT_INVALID;
    }

    for (int32_t i = qr_version + 1; i < qrcodegen_VERSION_MAX; i++) {
        if (qrcodegen_version2size(i) * scale > draw_buf->header.w) {
            break;
        }
        qr_version = i;
    }

    qr0 = lv_malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version));
    data_tmp = lv_malloc(qrcodegen_BUFFER_LEN_FOR_VERSION(qr_version));
    if ((qr0 == NULL) || (data_tmp == NULL)) {
        lv_free(qr0);
        lv_free(data_tmp);
        return LV_RESULT_INVALID;
    }
    memcpy(data_tmp, data, data_len);

    if (!qrcodegen_encodeBinary(data_tmp,
                                data_len,
                                qr0,
                                qrcodegen_Ecc_LOW,
                                qr_version,
                                qr_version,
                                qrcodegen_Mask_AUTO,
                                true)) {
        lv_free(qr0);
        lv_free(data_tmp);
        return LV_RESULT_INVALID;
    }

    lv_display_enable_invalidation(lv_obj_get_display(obj), false);

    int32_t obj_w = draw_buf->header.w;
    qr_size = qrcodegen_getSize(qr0);
    scale = obj_w / qr_size;
    int scaled = qr_size * scale;
    int margin = (obj_w - scaled) / 2;
    uint8_t *buf_u8 = (uint8_t *)draw_buf->data + 8;
    lv_color_t c = lv_color_hex(1);
    uint32_t row_byte_cnt = draw_buf->header.stride;

    for (int y = margin; y < scaled + margin; y += scale) {
        uint8_t b = 0;
        uint8_t p = 0;
        bool aligned = false;
        int x;

        for (x = margin; x < scaled + margin; x++) {
            bool module_is_dark = qrcodegen_getModule(qr0, (x - margin) / scale, (y - margin) / scale);

            if (!aligned && ((x & 0x7) == 0)) {
                aligned = true;
            }
            if (!aligned) {
                if (module_is_dark) {
                    lv_canvas_set_px(obj, x, y, c, LV_OPA_COVER);
                }
            } else {
                if (!module_is_dark) {
                    b |= (1 << (7 - p));
                }
                p++;
                if (p == 8) {
                    uint32_t px = row_byte_cnt * y + (x >> 3);
                    buf_u8[px] = (uint8_t)~b;
                    b = 0;
                    p = 0;
                }
            }
        }

        if (p) {
            uint32_t px;

            b |= (1 << (8 - p)) - 1;
            px = row_byte_cnt * y + (x >> 3);
            buf_u8[px] = (uint8_t)~b;
        }

        const uint8_t *row_ori = buf_u8 + row_byte_cnt * y;
        for (int s = 1; s < scale; s++) {
            memcpy((uint8_t *)buf_u8 + row_byte_cnt * (y + s), row_ori, row_byte_cnt);
        }
    }

    lv_display_enable_invalidation(lv_obj_get_display(obj), true);

    lv_free(qr0);
    lv_free(data_tmp);
    return LV_RESULT_OK;
}

static void display_lvgl_renderer_ensure_provisioning_objects(void)
{
    lv_obj_t *parent = objects.main;

    if ((s_provision_container != NULL) || (parent == NULL)) {
        return;
    }

    s_provision_container = lv_obj_create(parent);
    lv_obj_set_pos(s_provision_container, 0, 0);
    lv_obj_set_size(s_provision_container, 320, 240);
    lv_obj_set_style_bg_color(s_provision_container, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_provision_container, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_provision_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_provision_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_provision_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_provision_container, LV_OBJ_FLAG_HIDDEN);

    s_provision_wifi_icon = lv_image_create(s_provision_container);
    lv_obj_set_size(s_provision_wifi_icon, 34, 34);
    lv_obj_set_pos(s_provision_wifi_icon, 276, 4);

    s_provision_qr = lv_qrcode_create(s_provision_container);
    lv_qrcode_set_size(s_provision_qr, DISPLAY_LVGL_RENDERER_QR_SIZE);
    lv_qrcode_set_dark_color(s_provision_qr, lv_color_black());
    lv_qrcode_set_light_color(s_provision_qr, lv_color_white());
    lv_obj_set_pos(s_provision_qr,
                   (320 - DISPLAY_LVGL_RENDERER_QR_SIZE) / 2,
                   44);
    lv_obj_set_style_border_color(s_provision_qr, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_provision_qr, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    s_provision_message = lv_label_create(s_provision_container);
    lv_obj_set_width(s_provision_message, 300);
    lv_obj_set_pos(s_provision_message, 10, 196);
    lv_obj_set_style_text_align(s_provision_message, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_provision_message, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_provision_message,
                               &ui_font_source_han_sans_sc_normal_16,
                               LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(s_provision_message, LV_LABEL_LONG_WRAP);
    lv_label_set_text_static(s_provision_message, "");
}

static void display_lvgl_renderer_update_provisioning(const display_view_model_t *view_model)
{
    const char *payload;
    const char *message;
    bool visible;

    if (view_model == NULL) {
        return;
    }

    display_lvgl_renderer_ensure_provisioning_objects();
    if (s_provision_container == NULL) {
        return;
    }

    visible = (view_model->provisioning_state != DISPLAY_PROVISIONING_STATE_HIDDEN);
    if (!visible) {
        lv_obj_add_flag(s_provision_container, LV_OBJ_FLAG_HIDDEN);
        s_last_provisioning_state = DISPLAY_PROVISIONING_STATE_HIDDEN;
        return;
    }

    lv_obj_remove_flag(s_provision_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_provision_container);
    status_lvgl_image_update(s_provision_wifi_icon, &view_model->top_right_icon);

    payload = view_model->provisioning_qr_payload[0] == '\0' ? "{}" : view_model->provisioning_qr_payload;
    if (strcmp(s_last_qr_payload, payload) != 0) {
        lv_result_t qr_ret = display_lvgl_renderer_qrcode_update_low_ecc(s_provision_qr,
                                                                         payload,
                                                                         strlen(payload));
        if (qr_ret == LV_RESULT_OK) {
            snprintf(s_last_qr_payload, sizeof(s_last_qr_payload), "%s", payload);
        }
    }

    if (s_last_provisioning_state != view_model->provisioning_state) {
        message = display_lvgl_renderer_provisioning_message(view_model->provisioning_state);
        lv_label_set_text(s_provision_message, message);
        s_last_provisioning_state = view_model->provisioning_state;
    }
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
        display_lvgl_renderer_update_page(&view_model);
        display_lvgl_renderer_update_provisioning(&view_model);
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

    display_lvgl_renderer_update_page(&view_model);
    display_lvgl_renderer_update_provisioning(&view_model);
}

void display_lvgl_renderer_init(void)
{
    expression_lvgl_init(objects.main);
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
