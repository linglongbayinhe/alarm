#ifndef DISPLAY_LVGL_RENDERER_H
#define DISPLAY_LVGL_RENDERER_H

#include "display_view_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates an LVGL timer that refreshes EEZ UI widgets from application services.
 * Must be called inside the LVGL task context after ui_init() has populated
 * the objects table.
 */
void display_lvgl_renderer_init(void);

/**
 * Publishes the latest presenter output for LVGL widgets.
 *
 * May be called from the application/UI producer task. The LVGL timer copies
 * this cached model and renders it inside the LVGL task context.
 */
void display_lvgl_renderer_set_view_model(const display_view_model_t *view_model);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_LVGL_RENDERER_H */
