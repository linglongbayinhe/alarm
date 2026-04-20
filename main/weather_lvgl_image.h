#ifndef WEATHER_LVGL_IMAGE_H
#define WEATHER_LVGL_IMAGE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Updates the weather lv_image from the current snapshot (mock or future API).
 * WEATHER_ICON_CLEAR_DAY uses the embedded 64x64 bitmap; other icons use the
 * procedural renderer. Safe to call from the LVGL task only.
 */
void weather_lvgl_image_update(lv_obj_t *img);

#ifdef __cplusplus
}
#endif

#endif /* WEATHER_LVGL_IMAGE_H */
