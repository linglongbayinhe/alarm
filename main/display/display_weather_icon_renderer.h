#ifndef DISPLAY_WEATHER_ICON_RENDERER_H
#define DISPLAY_WEATHER_ICON_RENDERER_H

#include "display_service.h"
#include "display_status_icon_renderer.h"

void display_weather_icon_renderer_draw(const display_weather_panel_t *panel,
                                        display_canvas_t *canvas,
                                        int x_offset,
                                        int y_offset);

#endif
