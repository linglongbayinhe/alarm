#include "display_status_icon_renderer.h"

#include <stdbool.h>
#include <stddef.h>

#define DISPLAY_COLOR_WHITE 0xFFFF
#define DISPLAY_COLOR_RED 0xF800

#define WIFI_STATUS_ICON_SIZE 34
#define WIFI_STATUS_ICON_ORIGIN_OFFSET_X 18
#define WIFI_STATUS_ICON_ORIGIN_OFFSET_Y 17
#define WIFI_STATUS_ICON_BAND_COUNT 3
#define WIFI_STATUS_ICON_FIRST_OUTER_RADIUS 7
#define WIFI_STATUS_ICON_RADIUS_STEP 4
#define WIFI_STATUS_ICON_BAND_THICKNESS 2
#define WIFI_STATUS_ICON_DOT_RADIUS 2
#define WIFI_STATUS_ICON_SLASH_THICKNESS 3
#define WIFI_STATUS_ICON_SLASH_OFFSET_X -6

static int display_status_icon_abs_int(int value)
{
    return value < 0 ? -value : value;
}

static void display_status_icon_set_pixel(display_canvas_t *canvas, int x, int y, uint16_t color)
{
    if ((canvas == NULL) || (canvas->pixels == NULL)) {
        return;
    }
    if ((x < 0) || (x >= canvas->width) || (y < 0) || (y >= canvas->height)) {
        return;
    }

    canvas->pixels[(y * canvas->width) + x] = color;
}

static void display_status_icon_fill_rect(display_canvas_t *canvas,
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
            display_status_icon_set_pixel(canvas, current_x, current_y, color);
        }
    }
}

static void display_status_icon_draw_filled_circle(display_canvas_t *canvas,
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
                display_status_icon_set_pixel(canvas, center_x + column, center_y + row, color);
            }
        }
    }
}

static void display_status_icon_draw_wifi_sector_band(display_canvas_t *canvas,
                                                      int x_offset,
                                                      int y_offset,
                                                      int inner_radius,
                                                      int outer_radius,
                                                      uint16_t color)
{
    int local_x = 0;
    int local_y = 0;
    int origin_x = x_offset + WIFI_STATUS_ICON_ORIGIN_OFFSET_X;
    int origin_y = y_offset + WIFI_STATUS_ICON_ORIGIN_OFFSET_Y;
    int inner_radius_squared = inner_radius * inner_radius;
    int outer_radius_squared = outer_radius * outer_radius;

    for (local_y = 0; local_y < WIFI_STATUS_ICON_SIZE; ++local_y) {
        for (local_x = 0; local_x < WIFI_STATUS_ICON_SIZE; ++local_x) {
            int dx = local_x - WIFI_STATUS_ICON_ORIGIN_OFFSET_X;
            int dy = WIFI_STATUS_ICON_ORIGIN_OFFSET_Y - local_y;
            int distance_squared = 0;

            if (dy < 0) {
                continue;
            }
            if (display_status_icon_abs_int(dx) > dy) {
                continue;
            }

            distance_squared = (dx * dx) + (dy * dy);
            if ((distance_squared >= inner_radius_squared) &&
                (distance_squared <= outer_radius_squared)) {
                if ((dx == 0) && (dy == outer_radius)) {
                    continue;
                }
                display_status_icon_set_pixel(canvas, origin_x + dx, origin_y - dy, color);
            }
        }
    }
}

static void display_status_icon_draw_thick_line(display_canvas_t *canvas,
                                                int x0,
                                                int y0,
                                                int x1,
                                                int y1,
                                                int thickness,
                                                uint16_t color)
{
    int delta_x = display_status_icon_abs_int(x1 - x0);
    int delta_y = display_status_icon_abs_int(y1 - y0);
    int step_x = x0 < x1 ? 1 : -1;
    int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x - delta_y;
    int half_thickness = thickness / 2;

    while (true) {
        display_status_icon_fill_rect(canvas,
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

static uint8_t display_status_icon_get_wifi_band_count(const display_status_icon_t *icon)
{
    if (icon->level == 0U) {
        return WIFI_STATUS_ICON_BAND_COUNT;
    }
    if (icon->level > WIFI_STATUS_ICON_BAND_COUNT) {
        return WIFI_STATUS_ICON_BAND_COUNT;
    }

    return icon->level;
}

static void display_status_icon_draw_wifi_base(const display_status_icon_t *icon,
                                               display_canvas_t *canvas,
                                               int x_offset,
                                               int y_offset)
{
    uint8_t band_count = display_status_icon_get_wifi_band_count(icon);
    int band_index = 0;
    int outer_radius = 0;
    int inner_radius = 0;
    int dot_center_x = x_offset + WIFI_STATUS_ICON_ORIGIN_OFFSET_X;
    int dot_center_y = y_offset + WIFI_STATUS_ICON_ORIGIN_OFFSET_Y;

    display_status_icon_draw_filled_circle(canvas,
                                           dot_center_x,
                                           dot_center_y,
                                           WIFI_STATUS_ICON_DOT_RADIUS,
                                           DISPLAY_COLOR_WHITE);

    for (band_index = 0; band_index < band_count; ++band_index) {
        outer_radius = WIFI_STATUS_ICON_FIRST_OUTER_RADIUS + (band_index * WIFI_STATUS_ICON_RADIUS_STEP);
        inner_radius = outer_radius - WIFI_STATUS_ICON_BAND_THICKNESS;
        display_status_icon_draw_wifi_sector_band(canvas,
                                                  x_offset,
                                                  y_offset,
                                                  inner_radius,
                                                  outer_radius,
                                                  DISPLAY_COLOR_WHITE);
    }
}

static void display_status_icon_draw_wifi_alert_overlay(display_canvas_t *canvas, int x_offset, int y_offset)
{
    int slash_start_x = x_offset + WIFI_STATUS_ICON_ORIGIN_OFFSET_X + WIFI_STATUS_ICON_FIRST_OUTER_RADIUS +
                        ((WIFI_STATUS_ICON_BAND_COUNT - 1) * WIFI_STATUS_ICON_RADIUS_STEP) +
                        WIFI_STATUS_ICON_SLASH_OFFSET_X;
    int slash_start_y = y_offset + WIFI_STATUS_ICON_ORIGIN_OFFSET_Y -
                        (WIFI_STATUS_ICON_FIRST_OUTER_RADIUS +
                         ((WIFI_STATUS_ICON_BAND_COUNT - 1) * WIFI_STATUS_ICON_RADIUS_STEP)) + 1;
    int slash_end_x = x_offset + WIFI_STATUS_ICON_ORIGIN_OFFSET_X + WIFI_STATUS_ICON_SLASH_OFFSET_X;
    int slash_end_y = y_offset + WIFI_STATUS_ICON_ORIGIN_OFFSET_Y + 1;

    display_status_icon_draw_thick_line(canvas,
                                        slash_start_x,
                                        slash_start_y,
                                        slash_end_x,
                                        slash_end_y,
                                        WIFI_STATUS_ICON_SLASH_THICKNESS,
                                        DISPLAY_COLOR_RED);
}

void display_status_icon_renderer_draw(const display_status_icon_t *icon,
                                       display_canvas_t *canvas,
                                       int x_offset,
                                       int y_offset)
{
    if ((icon == NULL) || (canvas == NULL) || (canvas->pixels == NULL)) {
        return;
    }
    if (!icon->visible || (icon->kind == DISPLAY_STATUS_ICON_KIND_NONE)) {
        return;
    }
    if (icon->kind != DISPLAY_STATUS_ICON_KIND_WIFI) {
        return;
    }

    display_status_icon_draw_wifi_base(icon, canvas, x_offset, y_offset);

    if (icon->variant == DISPLAY_STATUS_ICON_VARIANT_ALERT) {
        display_status_icon_draw_wifi_alert_overlay(canvas, x_offset, y_offset);
    }
}
