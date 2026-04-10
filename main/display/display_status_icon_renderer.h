#ifndef DISPLAY_STATUS_ICON_RENDERER_H
#define DISPLAY_STATUS_ICON_RENDERER_H

#include <stdint.h>

#include "display_service.h"

typedef struct {
    uint16_t *pixels;
    int width;
    int height;
} display_canvas_t;

void display_status_icon_renderer_draw(const display_status_icon_t *icon,
                                       display_canvas_t *canvas,
                                       int x_offset,
                                       int y_offset);

#endif
