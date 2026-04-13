#include "display_weather_icon_renderer.h"

#include <stdbool.h>
#include <stddef.h>

#define DISPLAY_COLOR_SUN_YELLOW 0xFDC2

#define WEATHER_ICON_SIZE 40
#define WEATHER_SUN_CENTER_OFFSET_X 20
#define WEATHER_SUN_CENTER_OFFSET_Y 20
#define WEATHER_SUN_CORE_RADIUS 10
#define WEATHER_SUN_RAY_THICKNESS 2

typedef struct {
    int start_x;
    int start_y;
    int end_x;
    int end_y;
} display_weather_ray_t;

static const display_weather_ray_t WEATHER_SUN_RAYS[] = {
    {20, 7, 20, 2},
    {29, 11, 33, 7},
    {33, 20, 38, 20},
    {29, 29, 33, 33},
    {20, 33, 20, 38},
    {11, 29, 7, 33},
    {7, 20, 2, 20},
    {11, 11, 7, 7},
};

static int display_weather_icon_abs_int(int value)
{
    return value < 0 ? -value : value;
}

static void display_weather_icon_set_pixel(display_canvas_t *canvas, int x, int y, uint16_t color)
{
    if ((canvas == NULL) || (canvas->pixels == NULL)) {
        return;
    }
    if ((x < 0) || (x >= canvas->width) || (y < 0) || (y >= canvas->height)) {
        return;
    }

    canvas->pixels[(y * canvas->width) + x] = color;
}

static void display_weather_icon_fill_rect(display_canvas_t *canvas,
                                           int x,
                                           int y,
                                           int width,
                                           int height,
                                           uint16_t color)
{
    int current_x = 0;
    int current_y = 0;

    for (current_y = y; current_y < (y + height); ++current_y) {
        for (current_x = x; current_x < (x + width); ++current_x) {
            display_weather_icon_set_pixel(canvas, current_x, current_y, color);
        }
    }
}

static void display_weather_icon_draw_filled_circle(display_canvas_t *canvas,
                                                    int center_x,
                                                    int center_y,
                                                    int radius,
                                                    uint16_t color)
{
    int row = 0;
    int column = 0;
    int radius_squared = radius * radius;

    for (row = -radius; row <= radius; ++row) {
        for (column = -radius; column <= radius; ++column) {
            int distance_squared = (column * column) + (row * row);

            if (distance_squared <= radius_squared) {
                display_weather_icon_set_pixel(canvas, center_x + column, center_y + row, color);
            }
        }
    }
}

static void display_weather_icon_draw_thick_line(display_canvas_t *canvas,
                                                 int x0,
                                                 int y0,
                                                 int x1,
                                                 int y1,
                                                 int thickness,
                                                 uint16_t color)
{
    int delta_x = display_weather_icon_abs_int(x1 - x0);
    int delta_y = display_weather_icon_abs_int(y1 - y0);
    int step_x = x0 < x1 ? 1 : -1;
    int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x - delta_y;
    int half_thickness = thickness / 2;

    while (true) {
        display_weather_icon_fill_rect(canvas,
                                       x0 - half_thickness,
                                       y0 - half_thickness,
                                       thickness,
                                       thickness,
                                       color);

        if ((x0 == x1) && (y0 == y1)) {
            break;
        }

        if ((error * 2) > -delta_y) {
            error -= delta_y;
            x0 += step_x;
        }
        if ((error * 2) < delta_x) {
            error += delta_x;
            y0 += step_y;
        }
    }
}

static void display_weather_icon_draw_clear_day(display_canvas_t *canvas, int x_offset, int y_offset)
{
    size_t index = 0;
    int center_x = x_offset + WEATHER_SUN_CENTER_OFFSET_X;
    int center_y = y_offset + WEATHER_SUN_CENTER_OFFSET_Y;

    for (index = 0; index < (sizeof(WEATHER_SUN_RAYS) / sizeof(WEATHER_SUN_RAYS[0])); ++index) {
        display_weather_icon_draw_thick_line(canvas,
                                             x_offset + WEATHER_SUN_RAYS[index].start_x,
                                             y_offset + WEATHER_SUN_RAYS[index].start_y,
                                             x_offset + WEATHER_SUN_RAYS[index].end_x,
                                             y_offset + WEATHER_SUN_RAYS[index].end_y,
                                             WEATHER_SUN_RAY_THICKNESS,
                                             DISPLAY_COLOR_SUN_YELLOW);
    }

    display_weather_icon_draw_filled_circle(canvas,
                                            center_x,
                                            center_y,
                                            WEATHER_SUN_CORE_RADIUS,
                                            DISPLAY_COLOR_SUN_YELLOW);
}

void display_weather_icon_renderer_draw(const display_weather_panel_t *panel,
                                        display_canvas_t *canvas,
                                        int x_offset,
                                        int y_offset)
{
    if ((panel == NULL) || (canvas == NULL) || (canvas->pixels == NULL)) {
        return;
    }
    if (!panel->visible) {
        return;
    }
    if ((panel->icon != DISPLAY_WEATHER_ICON_KIND_CLEAR_DAY) ||
        ((x_offset + WEATHER_ICON_SIZE) > canvas->width) ||
        ((y_offset + WEATHER_ICON_SIZE) > canvas->height)) {
        return;
    }

    display_weather_icon_draw_clear_day(canvas, x_offset, y_offset);
}
