#include "display_weather_icon_renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_COLOR_SUN_YELLOW 0xFDC2
#define DISPLAY_COLOR_WHITE 0xFFFF
#define DISPLAY_COLOR_MOON DISPLAY_COLOR_SUN_YELLOW
#define DISPLAY_COLOR_CLOUD_LIGHT 0xC618
#define DISPLAY_COLOR_CLOUD_DARK 0x8410
#define DISPLAY_COLOR_RAIN_BLUE 0x04FF
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

static const display_weather_ray_t WEATHER_PARTLY_CLOUDY_SUN_RAYS[] = {
    {34, 24, 34, 13},
    {49, 31, 58, 22},
    {54, 45, 66, 45},
    {49, 59, 58, 68},
    {34, 66, 34, 78},
    {20, 59, 11, 68},
    {15, 45, 3, 45},
    {20, 31, 11, 22},
};

static int display_weather_icon_scale(int value)
{
    return ((value * DISPLAY_WEATHER_ICON_RENDER_SIZE) + (WEATHER_ICON_BASE_SIZE / 2)) /
           WEATHER_ICON_BASE_SIZE;
}

static int display_weather_icon_scale_size(int value)
{
    int scaled = display_weather_icon_scale(value);

    return scaled < 1 ? 1 : scaled;
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

static void display_weather_icon_draw_scaled_rect(display_canvas_t *canvas,
                                                  int x_offset,
                                                  int y_offset,
                                                  int x,
                                                  int y,
                                                  int width,
                                                  int height,
                                                  uint16_t color)
{
    display_weather_icon_fill_rect(canvas,
                                   x_offset + display_weather_icon_scale(x),
                                   y_offset + display_weather_icon_scale(y),
                                   display_weather_icon_scale_size(width),
                                   display_weather_icon_scale_size(height),
                                   color);
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

static void display_weather_icon_draw_cloud_shape(display_canvas_t *canvas,
                                                  int x_offset,
                                                  int y_offset,
                                                  uint16_t color)
{
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 34, 60, 18, color);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 52, 48, 24, color);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 72, 60, 18, color);
    display_weather_icon_draw_scaled_rect(canvas, x_offset, y_offset, 24, 58, 58, 21, color);
    display_weather_icon_draw_scaled_rect(canvas, x_offset, y_offset, 36, 51, 35, 28, color);
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

static void display_weather_icon_draw_cloudy(display_canvas_t *canvas, int x_offset, int y_offset)
{
    size_t index = 0;

    for (index = 0;
         index < (sizeof(WEATHER_PARTLY_CLOUDY_SUN_RAYS) / sizeof(WEATHER_PARTLY_CLOUDY_SUN_RAYS[0]));
         ++index) {
        display_weather_icon_draw_scaled_round_line(canvas,
                                                    x_offset,
                                                    y_offset,
                                                    WEATHER_PARTLY_CLOUDY_SUN_RAYS[index].start_x,
                                                    WEATHER_PARTLY_CLOUDY_SUN_RAYS[index].start_y,
                                                    WEATHER_PARTLY_CLOUDY_SUN_RAYS[index].end_x,
                                                    WEATHER_PARTLY_CLOUDY_SUN_RAYS[index].end_y,
                                                    5,
                                                    DISPLAY_COLOR_SUN_YELLOW);
    }

    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 34, 45, 16, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_cloud_shape(canvas, x_offset, y_offset, DISPLAY_COLOR_CLOUD_LIGHT);
}

static void display_weather_icon_draw_overcast(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 45, 56, 18, DISPLAY_COLOR_CLOUD_DARK);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 62, 51, 20, DISPLAY_COLOR_CLOUD_DARK);
    display_weather_icon_draw_scaled_rect(canvas, x_offset, y_offset, 34, 59, 52, 18, DISPLAY_COLOR_CLOUD_DARK);
    display_weather_icon_draw_cloud_shape(canvas, x_offset, y_offset, DISPLAY_COLOR_CLOUD_LIGHT);
}

static void display_weather_icon_draw_rain(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas, x_offset, y_offset, DISPLAY_COLOR_CLOUD_LIGHT);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 36, 76, 30, 94, 5, DISPLAY_COLOR_RAIN_BLUE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 53, 76, 47, 96, 5, DISPLAY_COLOR_RAIN_BLUE);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 70, 76, 64, 94, 5, DISPLAY_COLOR_RAIN_BLUE);
}

static void display_weather_icon_draw_thunderstorm(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas, x_offset, y_offset, DISPLAY_COLOR_CLOUD_LIGHT);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 54, 69, 44, 88, 8, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 44, 88, 57, 84, 8, DISPLAY_COLOR_SUN_YELLOW);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 57, 84, 47, 98, 8, DISPLAY_COLOR_SUN_YELLOW);
}

static void display_weather_icon_draw_snow(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas, x_offset, y_offset, DISPLAY_COLOR_CLOUD_LIGHT);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 34, 82, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 52, 92, 5, DISPLAY_COLOR_WHITE);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 70, 82, 5, DISPLAY_COLOR_WHITE);
}

static void display_weather_icon_draw_fog(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_cloud_shape(canvas, x_offset, y_offset, DISPLAY_COLOR_CLOUD_LIGHT);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 22, 75, 78, 75, 5, DISPLAY_COLOR_CLOUD_DARK);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 16, 86, 84, 86, 5, DISPLAY_COLOR_CLOUD_LIGHT);
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 28, 96, 72, 96, 5, DISPLAY_COLOR_CLOUD_DARK);
}

static void display_weather_icon_draw_loading(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 34, 50, 7, DISPLAY_COLOR_CLOUD_DARK);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 50, 50, 7, DISPLAY_COLOR_CLOUD_LIGHT);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 66, 50, 7, DISPLAY_COLOR_CLOUD_DARK);
}

static void display_weather_icon_draw_unknown(display_canvas_t *canvas, int x_offset, int y_offset)
{
    display_weather_icon_draw_scaled_round_line(canvas, x_offset, y_offset, 50, 25, 50, 62, 8, DISPLAY_COLOR_ERROR_RED);
    display_weather_icon_draw_scaled_circle(canvas, x_offset, y_offset, 50, 78, 6, DISPLAY_COLOR_ERROR_RED);
}

void display_weather_icon_renderer_draw(const display_weather_panel_t *panel,
                                        display_canvas_t *canvas,
                                        int x_offset,
                                        int y_offset)
{
    if ((panel == NULL) || (canvas == NULL) || (canvas->pixels == NULL)) {
        return;
    }
    if (!panel->visible || (panel->icon == DISPLAY_WEATHER_ICON_KIND_NONE)) {
        return;
    }
    if (((x_offset + DISPLAY_WEATHER_ICON_RENDER_SIZE) > canvas->width) ||
        ((y_offset + DISPLAY_WEATHER_ICON_RENDER_SIZE) > canvas->height)) {
        return;
    }

    switch (panel->icon) {
        case DISPLAY_WEATHER_ICON_KIND_CLEAR_DAY:
            display_weather_icon_draw_clear_day(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_CLEAR_NIGHT:
            display_weather_icon_draw_clear_night(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_PARTLY_CLOUDY_DAY:
        case DISPLAY_WEATHER_ICON_KIND_PARTLY_CLOUDY_NIGHT:
        case DISPLAY_WEATHER_ICON_KIND_CLOUDY:
            display_weather_icon_draw_cloudy(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_OVERCAST:
            display_weather_icon_draw_overcast(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_LIGHT_RAIN:
        case DISPLAY_WEATHER_ICON_KIND_RAIN:
            display_weather_icon_draw_rain(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_THUNDERSTORM:
            display_weather_icon_draw_thunderstorm(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_SNOW:
            display_weather_icon_draw_snow(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_FOG:
        case DISPLAY_WEATHER_ICON_KIND_WINDY:
            display_weather_icon_draw_fog(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_LOADING:
            display_weather_icon_draw_loading(canvas, x_offset, y_offset);
            break;
        case DISPLAY_WEATHER_ICON_KIND_UNKNOWN:
        default:
            display_weather_icon_draw_unknown(canvas, x_offset, y_offset);
            break;
    }
}
