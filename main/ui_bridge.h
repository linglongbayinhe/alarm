#ifndef UI_BRIDGE_H
#define UI_BRIDGE_H

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

#ifdef __cplusplus
}
#endif

#endif /* UI_BRIDGE_H */
