#ifndef EXPRESSION_LVGL_H
#define EXPRESSION_LVGL_H

#include <stdbool.h>

#include "display_view_model.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates the expression layer and its fixed LVGL objects. Safe to call once
 * from the LVGL task after the main screen has been created.
 */
void expression_lvgl_init(lv_obj_t *parent);

/**
 * Shows or hides the expression layer. Hidden expressions keep their objects
 * allocated but skip animation work.
 */
void expression_lvgl_set_visible(bool visible);

/**
 * Switches the expression animation mode. Safe to call from the LVGL task.
 */
void expression_lvgl_set_mode(display_expression_kind_t mode);

#ifdef __cplusplus
}
#endif

#endif /* EXPRESSION_LVGL_H */
