/**
 * @file display_lvgl_port_cfg.h
 *
 * Central place for `lvgl_port_display_cfg_t` values that match this board's
 * ST7789 wiring and orientation (see display_config.h).
 *
 * Cross-check sources:
 * - esp_lvgl_port README / API: lvgl_port_display_cfg_t in esp_lvgl_port_disp.h
 *   https://components.espressif.com/components/espressif/esp_lvgl_port
 * - ESP-IDF spi_lcd_touch example (LVGL 9 + custom flush in-tree; buffer sizing & RGB565 still relevant):
 *   https://github.com/espressif/esp-idf/tree/master/examples/peripherals/lcd/spi_lcd_touch
 *   It uses partial buffers (~20 lines), DMA-capable memory, and RGB565.
 *
 * Field choices for this project:
 * - hres / vres: logical size after panel rotation — 320x240 landscape (DISPLAY_WIDTH/HEIGHT).
 * - rotation.{swap_xy,mirror_x,mirror_y}: must match the same hardware mapping already used with
 *   esp_lcd_panel_swap_xy / esp_lcd_panel_mirror in display_service.c (DISPLAY_SWAP_XY, etc.).
 * - buffer_size: in **pixels** (not bytes), per esp_lvgl_port. Partial refresh: width * N lines.
 *   spi_lcd_touch uses ~1/10 screen; we use 24 lines (~10% of 240) as a similar trade-off on WROOM-32E.
 * - double_buffer: true — smoothness; costs 2x the pixel buffer RAM.
 * - flags.buff_dma: true — required for SPI DMA path with RGB565 (see port README "DMA buffer" note).
 * - flags.buff_spiram: false — no PSRAM on typical WROOM-32E; keep buffers in internal DMA-capable RAM.
 * - flags.sw_rotate: false — use hardware rotation via esp_lcd + rotation cfg (not software rotate).
 * - monochrome: false — color ST7789 RGB565.
 *
 * Color / byte order:
 * - LVGL 9 removed LV_COLOR_16_SWAP; panel RGB order & invert are handled in esp_lcd init.
 * - If colors look wrong, adjust panel BGR/RGB + invert in display_config.h first.
 */

#ifndef DISPLAY_LVGL_PORT_CFG_H
#define DISPLAY_LVGL_PORT_CFG_H

#include "display_config.h"
#include "esp_lvgl_port.h"

/** Lines per LVGL draw buffer (partial refresh). Increase for smoother redraw, decrease to save RAM. */
#define DISPLAY_LVGL_PORT_BUFFER_LINES 24

/** buffer_size for lvgl_port_display_cfg_t, in pixels (see esp_lvgl_port_disp.h). */
#define DISPLAY_LVGL_PORT_BUFFER_PIXELS (DISPLAY_WIDTH * DISPLAY_LVGL_PORT_BUFFER_LINES)

/**
 * Fill @p out_cfg for lvgl_port_add_disp(). Call after io_handle and panel_handle are created.
 */
static inline void display_lvgl_port_fill_display_cfg(esp_lcd_panel_io_handle_t io_handle,
                                                      esp_lcd_panel_handle_t panel_handle,
                                                      lvgl_port_display_cfg_t *out_cfg)
{
    if (out_cfg == NULL) {
        return;
    }

    *out_cfg = (lvgl_port_display_cfg_t){
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .control_handle = NULL,
        .buffer_size = DISPLAY_LVGL_PORT_BUFFER_PIXELS,
        .double_buffer = true,
        .trans_size = 0,
        .hres = DISPLAY_WIDTH,
        .vres = DISPLAY_HEIGHT,
        .monochrome = false,
        .rotation = {
            .swap_xy = DISPLAY_SWAP_XY,
            .mirror_x = DISPLAY_MIRROR_X,
            .mirror_y = DISPLAY_MIRROR_Y,
        },
        .rounder_cb = NULL,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };
}

#endif /* DISPLAY_LVGL_PORT_CFG_H */
