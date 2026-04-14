#ifndef DISPLAY_CANVAS_H
#define DISPLAY_CANVAS_H

#include <stdint.h>

typedef struct {
    uint16_t *pixels;
    int width;
    int height;
} display_canvas_t;

#endif
