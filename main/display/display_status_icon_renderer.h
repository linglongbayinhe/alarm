#ifndef DISPLAY_STATUS_ICON_RENDERER_H
#define DISPLAY_STATUS_ICON_RENDERER_H

#include "display_canvas.h"
#include "display_view_model.h"

void display_status_icon_renderer_draw(const display_wifi_status_icon_t *icon,
                                       display_canvas_t *canvas,
                                       int x_offset,
                                       int y_offset);

#endif
