#include "display_weather_icon_renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_view_model.h"

#define DISPLAY_COLOR_SUN_YELLOW 0xFDC2
#define DISPLAY_COLOR_WHITE 0xFFFF
#define DISPLAY_COLOR_MOON DISPLAY_COLOR_SUN_YELLOW
#define DISPLAY_COLOR_CLOUD_WHITE DISPLAY_COLOR_WHITE
#define DISPLAY_COLOR_FOG_GRAY 0xC618
#define DISPLAY_COLOR_DARK_GRAY 0x8410
#define DISPLAY_COLOR_RAIN_BLUE 0x04FF
#define DISPLAY_COLOR_DUST_LINE 0xD6B1
#define DISPLAY_COLOR_ERROR_RED 0xF800

#define WEATHER_ICON_BASE_SIZE 100
#define WEATHER_ICON_LOGICAL_SCALE 256
#define WEATHER_SUN_CORE_CENTER_X 50
#define WEATHER_SUN_CORE_CENTER_Y 50
#define WEATHER_SUN_CORE_RADIUS 25
#define WEATHER_SUN_RAY_THICKNESS 9
#define WEATHER_SUN_DIAGONAL_RAY_TIP_RADIUS 1
#define WEATHER_MOON_CENTER_X 55
#define WEATHER_MOON_CENTER_Y 53
#define WEATHER_MOON_RADIUS 35
#define WEATHER_MOON_CUTOUT_CENTER_X 73
#define WEATHER_MOON_CUTOUT_CENTER_Y 40
#define WEATHER_MOON_CUTOUT_RADIUS 33

typedef struct {
    int start_x;
    int start_y;
    int end_x;
    int end_y;
} display_weather_ray_t;

typedef struct {
    int x;
    int y;
} display_weather_point_t;

typedef struct {
    int center_x_q;
    int center_y_q;
    int64_t radius_squared_q;
} display_weather_circle_q_t;

/* Quadrant skip flags for partial ring drawing */
#define SKIP_QUADRANT_BOTTOM_LEFT 0x01
#define SKIP_QUADRANT_BOTTOM_RIGHT 0x02
#define SKIP_QUADRANT_TOP_LEFT 0x04
#define SKIP_QUADRANT_TOP_RIGHT 0x08

/* Coordinates use a 100x100 logical icon space and scale at draw time. */
static const display_weather_ray_t WEATHER_SUN_RAYS[] = {
    {50, 15, 50, 4},
    {75, 25, 83, 17},
    {85, 50, 96, 50},
    {75, 75, 83, 83},
    {50, 85, 50, 96},
    {25, 75, 17, 83},
    {15, 50, 4, 50},
    {25, 25, 17, 17},
};

static const display_weather_point_t WEATHER_SUN_DIAGONAL_RAY_TIP_POINTS[] = {
    {86, 13},
    {86, 86},
    {13, 86},
    {13, 13},
};

static const display_weather_point_t WEATHER_MOON_TRIM_POINTS[] = {
    {82, 70},
};

static const display_weather_ray_t WEATHER_CLOUDY_SUN_RAYS[] = {
    {68, 11, 68, 3},
    {86, 20, 92, 12},
    {92, 39, 99, 39},
    {50, 20, 43, 13},
};

static const display_weather_ray_t WEATHER_LIGHT_RAIN_LINES[] = {
    {51, 84, 47, 97},
};

static const display_weather_ray_t WEATHER_MODERATE_RAIN_LINES[] = {
    {42, 84, 38, 97},
    {61, 84, 57, 97},
};

static const display_weather_ray_t WEATHER_HEAVY_RAIN_LINES[] = {
    {35, 84, 30, 98},
    {52, 84, 47, 98},
    {69, 84, 64, 98},
};

static const display_weather_ray_t WEATHER_THUNDER_RAIN_LINES[] = {
    {31, 84, 27, 97},
    {75, 84, 70, 98},
};

static const display_weather_point_t WEATHER_THUNDER_BOLT_POINTS[] = {
    {48, 64},
    {66, 64},
    {57, 79},
    {68, 79},
    {46, 99},
    {51, 84},
    {40, 84},
};

static int display_weather_icon_scale(int value)
{
    return ((value * DISPLAY_WEATHER_ICON_RENDER_SIZE) + (WEATHER_ICON_BASE_SIZE / 2)) /
           WEATHER_ICON_BASE_SIZE;
}

static int display_weather_icon_pixel_center_to_logical_q(int pixel)
{
    return (((pixel * 2 + 1) * WEATHER_ICON_BASE_SIZE * WEATHER_ICON_LOGICAL_SCALE) /
            (DISPLAY_WEATHER_ICON_RENDER_SIZE * 2));
}

static int display_weather_icon_logical_to_q(int value)
{
    return value * WEATHER_ICON_LOGICAL_SCALE;
}

static display_weather_circle_q_t display_weather_icon_make_circle_q(int center_x, int center_y, int radius)
{
    int radius_q = display_weather_icon_logical_to_q(radius);
    display_weather_circle_q_t circle = {
        .center_x_q = display_weather_icon_logical_to_q(center_x),
        .center_y_q = display_weather_icon_logical_to_q(center_y),
        .radius_squared_q = (int64_t)radius_q * radius_q,
    };

    return circle;
}

static bool display_weather_icon_is_inside_circle_q(int pixel_x_q,
                                                    int pixel_y_q,
                                                    const display_weather_circle_q_t *circle)
{
    if (circle == NULL) {
        return false;
    }

    int64_t delta_x_q = pixel_x_q - circle->center_x_q;
    int64_t delta_y_q = pixel_y_q - circle->center_y_q;
    int64_t distance_squared_q = (delta_x_q * delta_x_q) + (delta_y_q * delta_y_q);

    return distance_squared_q <= circle->radius_squared_q;
}

static bool display_weather_icon_is_scaled_point(int local_x, int local_y, const display_weather_point_t *point)
{
    if (point == NULL) {
        return false;
    }

    return (local_x == display_weather_icon_scale(point->x)) &&
           (local_y == display_weather_icon_scale(point->y));
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

static void display_weather_icon_draw_logical_circle(display_canvas_t *canvas,
                                                     int x_offset,
                                                     int y_offset,
                                                     int center_x,
                                                     int center_y,
                                                     int radius,
                                                     uint16_t color)
{
    int local_x = 0;
    int local_y = 0;
    display_weather_circle_q_t circle = display_weather_icon_make_circle_q(center_x, center_y, radius);

    for (local_y = 0; local_y < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_y) {
        int pixel_y_q = display_weather_icon_pixel_center_to_logical_q(local_y);

        for (local_x = 0; local_x < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_x) {
            int pixel_x_q = display_weather_icon_pixel_center_to_logical_q(local_x);

            if (display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &circle)) {
                display_weather_icon_set_pixel(canvas, x_offset + local_x, y_offset + local_y, color);
            }
        }
    }
}

static void display_weather_icon_draw_logical_capsule(display_canvas_t *canvas,
                                                      int x_offset,
                                                      int y_offset,
                                                      int start_x,
                                                      int start_y,
                                                      int end_x,
                                                      int end_y,
                                                      int thickness,
                                                      uint16_t color)
{
    int local_x = 0;
    int local_y = 0;
    int64_t start_x_q = (int64_t)start_x * WEATHER_ICON_LOGICAL_SCALE;
    int64_t start_y_q = (int64_t)start_y * WEATHER_ICON_LOGICAL_SCALE;
    int64_t end_x_q = (int64_t)end_x * WEATHER_ICON_LOGICAL_SCALE;
    int64_t end_y_q = (int64_t)end_y * WEATHER_ICON_LOGICAL_SCALE;
    int64_t segment_x_q = end_x_q - start_x_q;
    int64_t segment_y_q = end_y_q - start_y_q;
    int64_t segment_length_squared_q = (segment_x_q * segment_x_q) + (segment_y_q * segment_y_q);
    int64_t radius_q = ((int64_t)thickness * WEATHER_ICON_LOGICAL_SCALE) / 2;
    int64_t radius_squared_q = radius_q * radius_q;

    if (segment_length_squared_q == 0) {
        display_weather_icon_draw_logical_circle(canvas, x_offset, y_offset, start_x, start_y, thickness / 2, color);
        return;
    }

    for (local_y = 0; local_y < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_y) {
        int64_t pixel_y_q = display_weather_icon_pixel_center_to_logical_q(local_y);

        for (local_x = 0; local_x < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_x) {
            int64_t pixel_x_q = display_weather_icon_pixel_center_to_logical_q(local_x);
            int64_t point_x_q = pixel_x_q - start_x_q;
            int64_t point_y_q = pixel_y_q - start_y_q;
            int64_t projection_q = (point_x_q * segment_x_q) + (point_y_q * segment_y_q);
            int64_t closest_x_q = 0;
            int64_t closest_y_q = 0;
            int64_t delta_x_q = 0;
            int64_t delta_y_q = 0;
            int64_t distance_squared_q = 0;

            if (projection_q <= 0) {
                closest_x_q = start_x_q;
                closest_y_q = start_y_q;
            } else if (projection_q >= segment_length_squared_q) {
                closest_x_q = end_x_q;
                closest_y_q = end_y_q;
            } else {
                closest_x_q = start_x_q + ((segment_x_q * projection_q) / segment_length_squared_q);
                closest_y_q = start_y_q + ((segment_y_q * projection_q) / segment_length_squared_q);
            }

            delta_x_q = pixel_x_q - closest_x_q;
            delta_y_q = pixel_y_q - closest_y_q;
            distance_squared_q = (delta_x_q * delta_x_q) + (delta_y_q * delta_y_q);

            if (distance_squared_q <= radius_squared_q) {
                display_weather_icon_set_pixel(canvas, x_offset + local_x, y_offset + local_y, color);
            }
        }
    }
}

static void display_weather_icon_draw_logical_ring(display_canvas_t *canvas,
                                                   int x_offset,
                                                   int y_offset,
                                                   int center_x,
                                                   int center_y,
                                                   int outer_radius,
                                                   int inner_radius,
                                                   uint16_t color)
{
    int local_x = 0;
    int local_y = 0;
    display_weather_circle_q_t outer_circle = display_weather_icon_make_circle_q(center_x, center_y, outer_radius);
    display_weather_circle_q_t inner_circle = display_weather_icon_make_circle_q(center_x, center_y, inner_radius);

    for (local_y = 0; local_y < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_y) {
        int pixel_y_q = display_weather_icon_pixel_center_to_logical_q(local_y);

        for (local_x = 0; local_x < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_x) {
            int pixel_x_q = display_weather_icon_pixel_center_to_logical_q(local_x);
            bool inside_outer = display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &outer_circle);
            bool inside_inner = display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &inner_circle);

            if (inside_outer && !inside_inner) {
                display_weather_icon_set_pixel(canvas, x_offset + local_x, y_offset + local_y, color);
            }
        }
    }
}

static void display_weather_icon_draw_logical_partial_ring(display_canvas_t *canvas,
                                                           int x_offset,
                                                           int y_offset,
                                                           int center_x,
                                                           int center_y,
                                                           int outer_radius,
                                                           int inner_radius,
                                                           uint8_t skip_quadrants,
                                                           uint16_t color)
{
    int local_x = 0;
    int local_y = 0;
    display_weather_circle_q_t outer_circle = display_weather_icon_make_circle_q(center_x, center_y, outer_radius);
    display_weather_circle_q_t inner_circle = display_weather_icon_make_circle_q(center_x, center_y, inner_radius);
    int center_x_q = display_weather_icon_logical_to_q(center_x);
    int center_y_q = display_weather_icon_logical_to_q(center_y);

    for (local_y = 0; local_y < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_y) {
        int pixel_y_q = display_weather_icon_pixel_center_to_logical_q(local_y);

        for (local_x = 0; local_x < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_x) {
            int pixel_x_q = display_weather_icon_pixel_center_to_logical_q(local_x);
            bool inside_outer = display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &outer_circle);
            bool inside_inner = display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &inner_circle);

            if (inside_outer && !inside_inner) {
                // Check which quadrant this pixel is in
                int delta_x_q = pixel_x_q - center_x_q;
                int delta_y_q = pixel_y_q - center_y_q;
                
                bool skip = false;
                
                // Check if in bottom-left quadrant (delta_x < 0, delta_y >= 0)
                if ((delta_x_q < 0 && delta_y_q >= 0) && (skip_quadrants & SKIP_QUADRANT_BOTTOM_LEFT)) {
                    skip = true;
                }
                // Check if in bottom-right quadrant (delta_x >= 0, delta_y >= 0)
                else if ((delta_x_q >= 0 && delta_y_q >= 0) && (skip_quadrants & SKIP_QUADRANT_BOTTOM_RIGHT)) {
                    skip = true;
                }
                // Check if in top-left quadrant (delta_x < 0, delta_y < 0)
                else if ((delta_x_q < 0 && delta_y_q < 0) && (skip_quadrants & SKIP_QUADRANT_TOP_LEFT)) {
                    skip = true;
                }
                // Check if in top-right quadrant (delta_x >= 0, delta_y < 0)
                else if ((delta_x_q >= 0 && delta_y_q < 0) && (skip_quadrants & SKIP_QUADRANT_TOP_RIGHT)) {
                    skip = true;
                }
                
                if (!skip) {
                    display_weather_icon_set_pixel(canvas, x_offset + local_x, y_offset + local_y, color);
                }
            }
        }
    }
}

static bool display_weather_icon_is_inside_polygon_q(int pixel_x_q,
                                                     int pixel_y_q,
                                                     const display_weather_point_t *points,
                                                     size_t point_count)
{
    bool inside = false;
    size_t current = 0;
    size_t previous = 0;

    if ((points == NULL) || (point_count < 3)) {
        return false;
    }

    previous = point_count - 1;
    for (current = 0; current < point_count; ++current) {
        int current_x_q = display_weather_icon_logical_to_q(points[current].x);
        int current_y_q = display_weather_icon_logical_to_q(points[current].y);
        int previous_x_q = display_weather_icon_logical_to_q(points[previous].x);
        int previous_y_q = display_weather_icon_logical_to_q(points[previous].y);
        bool crosses_y = (current_y_q > pixel_y_q) != (previous_y_q > pixel_y_q);

        if (crosses_y) {
            int64_t intersect_x_q = current_x_q +
                                    (((int64_t)(pixel_y_q - current_y_q) *
                                      (previous_x_q - current_x_q)) /
                                     (previous_y_q - current_y_q));

            if (pixel_x_q <= intersect_x_q) {
                inside = !inside;
            }
        }

        previous = current;
    }

    return inside;
}

static void display_weather_icon_draw_logical_polygon(display_canvas_t *canvas,
                                                      int x_offset,
                                                      int y_offset,
                                                      const display_weather_point_t *points,
                                                      size_t point_count,
                                                      uint16_t color)
{
    int local_x = 0;
    int local_y = 0;

    for (local_y = 0; local_y < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_y) {
        int pixel_y_q = display_weather_icon_pixel_center_to_logical_q(local_y);

        for (local_x = 0; local_x < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_x) {
            int pixel_x_q = display_weather_icon_pixel_center_to_logical_q(local_x);

            if (display_weather_icon_is_inside_polygon_q(pixel_x_q, pixel_y_q, points, point_count)) {
                display_weather_icon_set_pixel(canvas, x_offset + local_x, y_offset + local_y, color);
            }
        }
    }
}

static void display_weather_icon_draw_scaled_circle(display_canvas_t *canvas,
                                                    int x_offset,
                                                    int y_offset,
                                                    int center_x,
                                                    int center_y,
                                                    int radius,
                                                    uint16_t color)
{
    display_weather_icon_draw_logical_circle(canvas, x_offset, y_offset, center_x, center_y, radius, color);
}

static void display_weather_icon_draw_scaled_ring(display_canvas_t *canvas,
                                                  int x_offset,
                                                  int y_offset,
                                                  int center_x,
                                                  int center_y,
                                                  int outer_radius,
                                                  int inner_radius,
                                                  uint16_t color)
{
    display_weather_icon_draw_logical_ring(canvas,
                                           x_offset,
                                           y_offset,
                                           center_x,
                                           center_y,
                                           outer_radius,
                                           inner_radius,
                                           color);
}

static void display_weather_icon_draw_scaled_partial_ring(display_canvas_t *canvas,
                                                          int x_offset,
                                                          int y_offset,
                                                          int center_x,
                                                          int center_y,
                                                          int outer_radius,
                                                          int inner_radius,
                                                          uint8_t skip_quadrants,
                                                          uint16_t color)
{
    display_weather_icon_draw_logical_partial_ring(canvas,
                                                   x_offset,
                                                   y_offset,
                                                   center_x,
                                                   center_y,
                                                   outer_radius,
                                                   inner_radius,
                                                   skip_quadrants,
                                                   color);
}

static void display_weather_icon_draw_scaled_round_line(display_canvas_t *canvas,
                                                        int x_offset,
                                                        int y_offset,
                                                        int start_x,
                                                        int start_y,
                                                        int end_x,
                                                        int end_y,
                                                        int thickness,
                                                        uint16_t color)
{
    display_weather_icon_draw_logical_capsule(canvas,
                                              x_offset,
                                              y_offset,
                                              start_x,
                                              start_y,
                                              end_x,
                                              end_y,
                                              thickness,
                                              color);
}

static void display_weather_icon_draw_scaled_polygon(display_canvas_t *canvas,
                                                     int x_offset,
                                                     int y_offset,
                                                     const display_weather_point_t *points,
                                                     size_t point_count,
                                                     uint16_t color)
{
    display_weather_icon_draw_logical_polygon(canvas, x_offset, y_offset, points, point_count, color);
}

static void display_weather_icon_draw_cloud_shape(display_canvas_t *canvas,
                                                  int x_offset,
                                                  int y_offset,
                                                  int logical_x_offset,
                                                  int logical_y_offset,
                                                  uint16_t color)
{
    display_weather_icon_draw_scaled_circle(canvas,
                                            x_offset,
                                            y_offset,
                                            41 + logical_x_offset,
                                            60 + logical_y_offset,
                                            22,
                                            color);
    display_weather_icon_draw_scaled_circle(canvas,
                                            x_offset,
                                            y_offset,
                                            66 + logical_x_offset,
                                            65 + logical_y_offset,
                                            17,
                                            color);
    display_weather_icon_draw_scaled_round_line(canvas,
                                                x_offset,
                                                y_offset,
                                                25 + logical_x_offset,
                                                72 + logical_y_offset,
                                                78 + logical_x_offset,
                                                72 + logical_y_offset,
                                                20,
                                                color);
    display_weather_icon_draw_scaled_round_line(canvas,
                                                x_offset,
                                                y_offset,
                                                36 + logical_x_offset,
                                                62 + logical_y_offset,
                                                66 + logical_x_offset,
                                                62 + logical_y_offset,
                                                21,
                                                color);
}

static void display_weather_icon_draw_crescent_shape(display_canvas_t *canvas,
                                                     int x_offset,
                                                     int y_offset,
                                                     int center_x,
                                                     int center_y,
                                                     int radius,
                                                     int cutout_center_x,
                                                     int cutout_center_y,
                                                     int cutout_radius,
                                                     uint16_t color)
{
    int local_x = 0;
    int local_y = 0;
    display_weather_circle_q_t moon_circle = display_weather_icon_make_circle_q(center_x, center_y, radius);
    display_weather_circle_q_t cutout_circle =
        display_weather_icon_make_circle_q(cutout_center_x, cutout_center_y, cutout_radius);

    for (local_y = 0; local_y < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_y) {
        int pixel_y_q = display_weather_icon_pixel_center_to_logical_q(local_y);

        for (local_x = 0; local_x < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_x) {
            int pixel_x_q = display_weather_icon_pixel_center_to_logical_q(local_x);
            bool is_moon_pixel = display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &moon_circle);
            bool is_cutout_pixel = display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &cutout_circle);

            if (is_moon_pixel && !is_cutout_pixel) {
                display_weather_icon_set_pixel(canvas, x_offset + local_x, y_offset + local_y, color);
            }
        }
    }
}

static void display_weather_icon_draw_rain_lines(display_canvas_t *canvas,
                                                 int x_offset,
                                                 int y_offset,
                                                 const display_weather_ray_t *rain_lines,
                                                 size_t rain_line_count)
{
    size_t index = 0;

    for (index = 0; index < rain_line_count; ++index) {
        display_weather_icon_draw_scaled_round_line(canvas,
                                                    x_offset,
                                                    y_offset,
                                                    rain_lines[index].start_x,
                                                    rain_lines[index].start_y,
                                                    rain_lines[index].end_x,
                                                    rain_lines[index].end_y,
                                                    7,
                                                    DISPLAY_COLOR_RAIN_BLUE);
    }
}

static void display_weather_icon_draw_raindrop(display_canvas_t *canvas,
                                               int x_offset,
                                               int y_offset,
                                               int center_x,
                                               int center_y)
{
    const display_weather_point_t drop_points[] = {
        {center_x, center_y - 9},
        {center_x - 6, center_y + 1},
        {center_x, center_y + 9},
        {center_x + 6, center_y + 1},
    };

    display_weather_icon_draw_scaled_polygon(canvas,
                                             x_offset,
                                             y_offset,
                                             drop_points,
                                             sizeof(drop_points) / sizeof(drop_points[0]),
                                             DISPLAY_COLOR_RAIN_BLUE);
    display_weather_icon_draw_scaled_circle(canvas,
                                            x_offset,
                                            y_offset,
                                            center_x,
                                            center_y + 2,
                                            5,
                                            DISPLAY_COLOR_RAIN_BLUE);
}

static void display_weather_icon_draw_clear_day(display_canvas_t *canvas, int x_offset, int y_offset)
{
    size_t index = 0;

    for (index = 0; index < (sizeof(WEATHER_SUN_RAYS) / sizeof(WEATHER_SUN_RAYS[0])); ++index) {
        display_weather_icon_draw_scaled_round_line(canvas,
                                                    x_offset,
                                                    y_offset,
                                                    WEATHER_SUN_RAYS[index].start_x,
                                                    WEATHER_SUN_RAYS[index].start_y,
                                                    WEATHER_SUN_RAYS[index].end_x,
                                                    WEATHER_SUN_RAYS[index].end_y,
                                                    WEATHER_SUN_RAY_THICKNESS,
                                                    DISPLAY_COLOR_SUN_YELLOW);
    }

    for (index = 0;
         index < (sizeof(WEATHER_SUN_DIAGONAL_RAY_TIP_POINTS) /
                  sizeof(WEATHER_SUN_DIAGONAL_RAY_TIP_POINTS[0]));
         ++index) {
        display_weather_icon_draw_scaled_circle(canvas,
                                                x_offset,
                                                y_offset,
                                                WEATHER_SUN_DIAGONAL_RAY_TIP_POINTS[index].x,
                                                WEATHER_SUN_DIAGONAL_RAY_TIP_POINTS[index].y,
                                                WEATHER_SUN_DIAGONAL_RAY_TIP_RADIUS,
                                                DISPLAY_COLOR_SUN_YELLOW);
    }

    display_weather_icon_draw_scaled_circle(canvas,
                                            x_offset,
                                            y_offset,
                                            WEATHER_SUN_CORE_CENTER_X,
                                            WEATHER_SUN_CORE_CENTER_Y,
                                            WEATHER_SUN_CORE_RADIUS,
                                            DISPLAY_COLOR_SUN_YELLOW);
}

static void display_weather_icon_draw_clear_night(display_canvas_t *canvas, int x_offset, int y_offset)
{
    int local_x = 0;
    int local_y = 0;
    display_weather_circle_q_t moon_circle =
        display_weather_icon_make_circle_q(WEATHER_MOON_CENTER_X, WEATHER_MOON_CENTER_Y, WEATHER_MOON_RADIUS);
    display_weather_circle_q_t cutout_circle = display_weather_icon_make_circle_q(WEATHER_MOON_CUTOUT_CENTER_X,
                                                                                  WEATHER_MOON_CUTOUT_CENTER_Y,
                                                                                  WEATHER_MOON_CUTOUT_RADIUS);

    for (local_y = 0; local_y < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_y) {
        int pixel_y_q = display_weather_icon_pixel_center_to_logical_q(local_y);

        for (local_x = 0; local_x < DISPLAY_WEATHER_ICON_RENDER_SIZE; ++local_x) {
            int pixel_x_q = display_weather_icon_pixel_center_to_logical_q(local_x);
            bool is_moon_pixel = display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &moon_circle);
            bool is_cutout_pixel = display_weather_icon_is_inside_circle_q(pixel_x_q, pixel_y_q, &cutout_circle);
            bool is_trim_pixel = false;
            size_t trim_index = 0;

            for (trim_index = 0; trim_index < (sizeof(WEATHER_MOON_TRIM_POINTS) / sizeof(WEATHER_MOON_TRIM_POINTS[0]));
                 ++trim_index) {
                if (display_weather_icon_is_scaled_point(local_x, local_y, &WEATHER_MOON_TRIM_POINTS[trim_index])) {
                    is_trim_pixel = true;
                    break;
                }
            }

            if (is_moon_pixel && !is_cutout_pixel && !is_trim_pixel) {
                display_weather_icon_set_pixel(canvas, x_offset + local_x, y_offset + local_y, DISPLAY_COLOR_MOON);
            }
        }
    }
}

static void display_weather_icon_draw_cloudy_day(display_canvas_t *canvas, int x_offset, int y_offset)
{
    size_t index = 0;

    for (index = 0;
         index < (sizeof(WEATHER_CLOUDY_SUN_RAYS) / sizeof(WEATHER_CLOUDY_SUN_RAYS[0]));
         ++index) {
        display_weather_icon_draw_scaled_round_line(canvas,
                                                    x_offset,
                                                    y_offset,
                                                    WEATHER_CLOUDY_SUN_RAYS[index].start_x,
                                                    WEATHER_CLOUDY_SUN_RAYS[index].start_y,
                                                    WEATHER_CLOUDY_SUN_RAYS[index].end_x,
                                                    WEATHER_CLOUDY_SUN_RAYS[index].end_y,
                                                    5,
                                                    DISPLAY_COLOR_SUN_YELLOW);
    }

    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 68, 39, 21, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          2,
                                          DISPLAY_COLOR_CLOUD_WHITE);
}

static void display_weather_icon_draw_cloudy_night(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_crescent_shape(canvas,
                                             x_offset,
                                             y_offset,
                                             68,
                                             39,
                                             27,
                                             83,
                                             29,
                                             26,
                                             DISPLAY_COLOR_MOON);
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          2,
                                          DISPLAY_COLOR_CLOUD_WHITE);
}

static void display_weather_icon_draw_overcast(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          2,
                                          DISPLAY_COLOR_CLOUD_WHITE);
}

static void display_weather_icon_draw_light_rain(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_rain_lines(canvas,
                                         x_offset,
                                         y_offset,
                                         WEATHER_LIGHT_RAIN_LINES,
                                         sizeof(WEATHER_LIGHT_RAIN_LINES) /
                                         sizeof(WEATHER_LIGHT_RAIN_LINES[0]));
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          -6,
                                          DISPLAY_COLOR_CLOUD_WHITE);
}

static void display_weather_icon_draw_moderate_rain(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_rain_lines(canvas,
                                         x_offset,
                                         y_offset,
                                         WEATHER_MODERATE_RAIN_LINES,
                                         sizeof(WEATHER_MODERATE_RAIN_LINES) /
                                         sizeof(WEATHER_MODERATE_RAIN_LINES[0]));
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          -6,
                                          DISPLAY_COLOR_CLOUD_WHITE);
}

static void display_weather_icon_draw_heavy_rain(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_rain_lines(canvas,
                                         x_offset,
                                         y_offset,
                                         WEATHER_HEAVY_RAIN_LINES,
                                         sizeof(WEATHER_HEAVY_RAIN_LINES) /
                                         sizeof(WEATHER_HEAVY_RAIN_LINES[0]));
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          -6,
                                          DISPLAY_COLOR_CLOUD_WHITE);
}

static void display_weather_icon_draw_shower(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          -7,
                                          DISPLAY_COLOR_CLOUD_WHITE);
    display_weather_icon_draw_raindrop(canvas, x_offset, y_offset, 35, 87);
    display_weather_icon_draw_raindrop(canvas, x_offset, y_offset, 52, 90);
    display_weather_icon_draw_raindrop(canvas, x_offset, y_offset, 69, 87);
}

static void display_weather_icon_draw_thunderstorm(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          -7,
                                          DISPLAY_COLOR_CLOUD_WHITE);
    display_weather_icon_draw_scaled_polygon(canvas,
                                             x_offset,
                                             y_offset,
                                             WEATHER_THUNDER_BOLT_POINTS,
                                             sizeof(WEATHER_THUNDER_BOLT_POINTS) /
                                             sizeof(WEATHER_THUNDER_BOLT_POINTS[0]),
                                             DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_rain_lines(canvas,
                                         x_offset,
                                         y_offset,
                                         WEATHER_THUNDER_RAIN_LINES,
                                         sizeof(WEATHER_THUNDER_RAIN_LINES) /
                                         sizeof(WEATHER_THUNDER_RAIN_LINES[0]));
}

static void display_weather_icon_draw_snow(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 50, 15, 50, 85, 7, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 16, 50, 84, 50, 7, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 27, 27, 73, 73, 7, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 73, 27, 27, 73, 7, DISPLAY_COLOR_WHITE);

    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 50, 31, 38, 22, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 50, 31, 62, 22, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 69, 50, 82, 39, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 69, 50, 82, 61, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 50, 69, 38, 78, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 50, 69, 62, 78, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 31, 50, 18, 39, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 31, 50, 18, 61, 5, DISPLAY_COLOR_WHITE);
}

static void display_weather_icon_draw_fog(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          -7,
                                          DISPLAY_COLOR_CLOUD_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 29, 84, 56, 84, 6, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 71, 84, 79, 84, 6, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 29, 96, 37, 96, 6, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 51, 96, 79, 96, 6, DISPLAY_COLOR_WHITE);
}

static void display_weather_icon_draw_haze(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_scaled_ring(canvas, x_offset, y_offset, 35, 52, 19, 13, DISPLAY_COLOR_FOG_GRAY);
    display_weather_icon_draw_scaled_ring(canvas, x_offset, y_offset, 65, 52, 19, 13, DISPLAY_COLOR_FOG_GRAY);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 48, 52, 52, 52, 6, DISPLAY_COLOR_FOG_GRAY);

    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 20, 24, 4, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 40, 24, 4, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 60, 24, 4, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 80, 24, 4, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 20, 78, 4, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 40, 78, 4, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 60, 78, 4, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 80, 78, 4, DISPLAY_COLOR_SUN_YELLOW);
}

static void display_weather_icon_draw_dust_storm(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 25, 45, 70, 45, 6, DISPLAY_COLOR_DUST_LINE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 22, 60, 70, 60, 6, DISPLAY_COLOR_DUST_LINE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 37, 75, 63, 75, 6, DISPLAY_COLOR_DUST_LINE);

    // Partial ring - bottom-left quadrant skipped, bottom point at (70, 45)
    display_weather_icon_draw_scaled_partial_ring(canvas, x_offset, y_offset, 70, 30, 15, 10, SKIP_QUADRANT_BOTTOM_LEFT, DISPLAY_COLOR_DUST_LINE);

    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 15, 45, 3, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 80, 60, 3, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 25, 75, 3, DISPLAY_COLOR_SUN_YELLOW);
}

static void display_weather_icon_draw_windy(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 23, 35, 57, 35, 6, DISPLAY_COLOR_RAIN_BLUE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 8, 50, 78, 50, 6, DISPLAY_COLOR_RAIN_BLUE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 14, 65, 48, 65, 6, DISPLAY_COLOR_RAIN_BLUE);

    // Partial ring - bottom-left quadrant skipped, bottom point at (57, 35)
    display_weather_icon_draw_scaled_partial_ring(canvas, x_offset, y_offset, 57, 26, 10, 6, SKIP_QUADRANT_BOTTOM_LEFT, DISPLAY_COLOR_RAIN_BLUE);
    // Partial ring - bottom-left quadrant skipped, bottom point at (78, 50)
    display_weather_icon_draw_scaled_partial_ring(canvas, x_offset, y_offset, 77, 64, 15, 10, SKIP_QUADRANT_TOP_LEFT, DISPLAY_COLOR_RAIN_BLUE);
}

static void display_weather_icon_draw_unknown(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas,
                                          x_offset,
                                          y_offset,
                                          0,
                                          0,
                                          DISPLAY_COLOR_CLOUD_WHITE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 50, 48, 50, 66, 8, DISPLAY_COLOR_ERROR_RED);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 50, 78, 5, DISPLAY_COLOR_ERROR_RED);
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
    if (((x_offset + DISPLAY_WEATHER_ICON_RENDER_SIZE) > canvas->width) ||
        ((y_offset + DISPLAY_WEATHER_ICON_RENDER_SIZE) > canvas->height)) {
        return;
    }

    switch (panel->icon) {
        case WEATHER_ICON_CLEAR_DAY:
            display_weather_icon_draw_clear_day(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_CLEAR_NIGHT:
            display_weather_icon_draw_clear_night(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_CLOUDY_DAY:
            display_weather_icon_draw_cloudy_day(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_CLOUDY_NIGHT:
            display_weather_icon_draw_cloudy_night(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_OVERCAST:
            display_weather_icon_draw_overcast(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_LIGHT_RAIN:
            display_weather_icon_draw_light_rain(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_MODERATE_RAIN:
            display_weather_icon_draw_moderate_rain(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_HEAVY_RAIN:
            display_weather_icon_draw_heavy_rain(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_SHOWER:
            display_weather_icon_draw_shower(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_THUNDERSTORM:
            display_weather_icon_draw_thunderstorm(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_SNOW:
            display_weather_icon_draw_snow(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_FOG:
            display_weather_icon_draw_fog(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_HAZE:
            display_weather_icon_draw_haze(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_DUST_STORM:
            display_weather_icon_draw_dust_storm(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_WINDY:
            display_weather_icon_draw_windy(canvas, x_offset, y_offset);
            break;
        case WEATHER_ICON_UNKNOWN:
        default:
            display_weather_icon_draw_unknown(canvas, x_offset, y_offset);
            break;
    }
}
