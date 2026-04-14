#ifndef DISPLAY_WEATHER_ICON_RENDERER_H
#define DISPLAY_WEATHER_ICON_RENDERER_H

#include "display_canvas.h"
#include "display_weather_icon_types.h"

#define DISPLAY_WEATHER_ICON_RENDER_SIZE 40

struct display_weather_panel;

void display_weather_icon_renderer_draw(const struct display_weather_panel *panel,
                                        display_canvas_t *canvas,
                                        int x_offset,
                                        int y_offset);

#endif
