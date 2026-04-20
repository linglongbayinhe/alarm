#ifndef WEATHER_LVGL_IMAGE_H
#define WEATHER_LVGL_IMAGE_H

#include "display_view_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Updates the weather lv_image from presenter output.
 * Uploaded bitmap icons use their embedded LVGL assets; other icons use the
 * procedural renderer fallback. Safe to call from the LVGL task only.
 */
void weather_lvgl_image_update(lv_obj_t *img, const display_weather_panel_t *panel);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_LVGL_IMAGE_H */
