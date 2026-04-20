#include "display_lvgl.h"

#include "display_config.h"
#include "display_lvgl_port_cfg.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "ui.h"
#include "ui_bridge.h"

static const char *TAG = "display_lvgl";

/** Solid RGB565 fill before LVGL draws, to remove previous firmware’s panel retention. */
#define DISPLAY_LVGL_STARTUP_CLEAR_RGB565 0x0000U

static void display_lvgl_fill_panel_solid(esp_lcd_panel_handle_t panel, uint16_t rgb565)
{
    uint16_t line[DISPLAY_WIDTH];
    int x;
    int y;

    for (x = 0; x < DISPLAY_WIDTH; x++) {
        line[x] = rgb565;
    }
    for (y = 0; y < DISPLAY_HEIGHT; y++) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, y, DISPLAY_WIDTH, y + 1, line);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "startup panel clear failed at y=%d: %s", y, esp_err_to_name(err));
            break;
        }
    }
}

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

    /* Full hardware clear so partial LVGL buffers do not leave last-flash garbage on panel. */
    display_lvgl_fill_panel_solid(panel, DISPLAY_LVGL_STARTUP_CLEAR_RGB565);

    ui_init();
    ui_bridge_init();

    /* Partial buffer: invalidate active screen so first flush covers full logical screen. */
    {
        lv_obj_t *scr = lv_display_get_screen_active(s_lvgl_disp);
        if (scr != NULL) {
            lv_obj_invalidate(scr);
        }
    }

    lvgl_port_unlock();

    s_lvgl_ready = true;
    ESP_LOGI(TAG, "EEZ UI initialized");
    return ESP_OK;
}
