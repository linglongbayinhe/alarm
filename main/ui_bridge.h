#ifndef UI_BRIDGE_H
#define UI_BRIDGE_H

#include "display_view_model.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates an LVGL timer that refreshes EEZ UI widgets from application
 * services (time, weather, …).  Must be called inside the LVGL task
 * context (i.e. between lvgl_port_lock / lvgl_port_unlock, or from an
 * lv_timer callback) **after** ui_init() has populated the objects table.
 */
void ui_bridge_init(void);

/**
 * Publishes the latest presenter output for LVGL widgets.
 *
 * May be called from the application/UI producer task. The LVGL timer copies
 * this cached model and renders it inside the LVGL task context.
 */
void ui_bridge_set_view_model(const display_view_model_t *view_model);

#ifdef __cplusplus
}
#endif

#endif /* UI_BRIDGE_H */
