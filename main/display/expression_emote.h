#ifndef EXPRESSION_EMOTE_H
#define EXPRESSION_EMOTE_H

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Create the LVGL surface and start the Emote initialization worker.
 *
 * Must be called from the LVGL task context after the UI parent exists.
 */
esp_err_t expression_emote_init(lv_obj_t *parent);

/** Show or hide the Emote expression page and control its blink schedule. */
void expression_emote_set_visible(bool visible);

#ifdef __cplusplus
}
#endif

#endif /* EXPRESSION_EMOTE_H */
