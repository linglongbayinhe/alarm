#include "display_lvgl.h"

#include "display_lvgl_port_cfg.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "fonts/app_fonts.h"
#include "lvgl.h"

static const char *TAG = "display_lvgl";

static lv_display_t *s_lvgl_disp;
static bool s_lvgl_ready;

bool display_lvgl_is_active(void)
{
    return s_lvgl_ready;
}

esp_err_t display_lvgl_init(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel)
{
    if (s_lvgl_ready) {
        return ESP_OK;
    }
    if ((panel_io == NULL) || (panel == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init failed");

    lvgl_port_display_cfg_t disp_cfg;
    display_lvgl_port_fill_display_cfg(panel_io, panel, &disp_cfg);

    s_lvgl_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_lvgl_disp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        (void)lvgl_port_deinit();
        return ESP_FAIL;
    }

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_display_get_screen_active(s_lvgl_disp);
    lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

    lv_obj_t *label_en = lv_label_create(scr);
    lv_label_set_text(label_en, "LVGL 9 + esp_lvgl_port OK");
    lv_obj_set_style_text_color(label_en, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(label_en, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *label_cn = lv_label_create(scr);
    lv_obj_set_style_text_font(label_cn, &SourceHanSans_Normal_16, LV_PART_MAIN);
    lv_label_set_text(label_cn, "周四晴25℃");
    lv_obj_set_style_text_color(label_cn, lv_color_white(), LV_PART_MAIN);
    lv_obj_align(label_cn, LV_ALIGN_CENTER, 0, -16);

    lv_obj_t *bar = lv_bar_create(scr);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 70, LV_ANIM_OFF);
    lv_obj_set_size(bar, 200, 18);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x00aaff), LV_PART_INDICATOR);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -16);

    lvgl_port_unlock();

    s_lvgl_ready = true;
    ESP_LOGI(TAG, "Phase A demo UI created (label + bar)");
    return ESP_OK;
}
