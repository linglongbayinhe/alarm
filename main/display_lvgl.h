#ifndef DISPLAY_LVGL_H
#define DISPLAY_LVGL_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

/**
 * @brief Start LVGL + esp_lvgl_port on an already initialized ST7789 (esp_lcd handles).
 *
 * Registers the display with lvgl_port_add_disp() using settings from display_lvgl_port_cfg.h.
 * Creates a minimal demo (label + bar) under lvgl_port_lock for on-screen verification (phase A).
 */
esp_err_t display_lvgl_init(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel);

/** True after display_lvgl_init() succeeds. Used to skip legacy strip renderer / unsafe raw flush. */
bool display_lvgl_is_active(void);

#endif /* DISPLAY_LVGL_H */
