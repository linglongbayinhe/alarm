#include "status_lvgl_image.h"

#include <stdbool.h>
#include <stdint.h>

#include "display_canvas.h"

#define STATUS_LVGL_ICON_SIDE 34
#define STATUS_LVGL_ICON_PIXELS (STATUS_LVGL_ICON_SIDE * STATUS_LVGL_ICON_SIDE)
#define STATUS_LVGL_ICON_BYTES  (STATUS_LVGL_ICON_PIXELS * (int)sizeof(uint16_t))
#define STATUS_LVGL_COLOR_BLACK 0x0000
#define STATUS_LVGL_COLOR_RED   0xF800
#define STATUS_LVGL_WIFI_ORIGIN_OFFSET_X 18
#define STATUS_LVGL_WIFI_ORIGIN_OFFSET_Y 17
#define STATUS_LVGL_WIFI_BAND_COUNT 3
#define STATUS_LVGL_WIFI_FIRST_OUTER_RADIUS 7
#define STATUS_LVGL_WIFI_RADIUS_STEP 4
#define STATUS_LVGL_WIFI_BAND_THICKNESS 2
#define STATUS_LVGL_WIFI_DOT_RADIUS 2
#define STATUS_LVGL_WIFI_SLASH_THICKNESS 3
#define STATUS_LVGL_WIFI_SLASH_OFFSET_X -6

static uint16_t s_pixels[STATUS_LVGL_ICON_PIXELS];
static lv_image_dsc_t s_img_dsc;
static bool s_inited;
static bool s_cached_valid;
static display_wifi_status_icon_t s_cached_icon;

static void status_lvgl_image_init_dsc(void)
{
    if (s_inited) {
        return;
    }

    s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w = STATUS_LVGL_ICON_SIDE;
    s_img_dsc.header.h = STATUS_LVGL_ICON_SIDE;
    s_img_dsc.header.stride = 0;
    s_img_dsc.data = (const uint8_t *)s_pixels;
    s_img_dsc.data_size = (uint32_t)STATUS_LVGL_ICON_BYTES;
    s_inited = true;
}

static bool status_lvgl_icon_equals(const display_wifi_status_icon_t *left,
                                    const display_wifi_status_icon_t *right)
{
    return (left->visible == right->visible) &&
           (left->kind == right->kind) &&
           (left->variant == right->variant) &&
           (left->level == right->level);
}

static void status_lvgl_fill_pixels_parent_bg(lv_obj_t *img)
{
    lv_obj_t *parent = lv_obj_get_parent(img);
    lv_color_t bg = lv_color_white();
    uint16_t fill_u16 = 0;

    if (parent != NULL) {
        bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
    }

    fill_u16 = lv_color_to_u16(bg);
    for (int i = 0; i < STATUS_LVGL_ICON_PIXELS; i++) {
        s_pixels[i] = fill_u16;
    }
}

static int status_lvgl_abs_int(int value)
{
    return value < 0 ? -value : value;
}

static void status_lvgl_set_pixel(display_canvas_t *canvas, int x, int y, uint16_t color)
{
    if ((canvas == NULL) || (canvas->pixels == NULL)) {
        return;
    }
    if ((x < 0) || (x >= canvas->width) || (y < 0) || (y >= canvas->height)) {
        return;
    }

    canvas->pixels[(y * canvas->width) + x] = color;
}

static void status_lvgl_fill_rect(display_canvas_t *canvas,
                                  int x,
                                  int y,
                                  int width,
                                  int height,
                                  uint16_t color)
{
    for (int current_y = y; current_y < (y + height); ++current_y) {
        for (int current_x = x; current_x < (x + width); ++current_x) {
            status_lvgl_set_pixel(canvas, current_x, current_y, color);
        }
    }
}

static void status_lvgl_draw_filled_circle(display_canvas_t *canvas,
                                           int center_x,
                                           int center_y,
                                           int radius,
                                           uint16_t color)
{
    int radius_squared = radius * radius;

    for (int row = -radius; row <= radius; ++row) {
        for (int column = -radius; column <= radius; ++column) {
            int distance_squared = (column * column) + (row * row);

            if (distance_squared <= radius_squared) {
                status_lvgl_set_pixel(canvas, center_x + column, center_y + row, color);
            }
        }
    }
}

static void status_lvgl_draw_wifi_sector_band(display_canvas_t *canvas,
                                              int inner_radius,
                                              int outer_radius,
                                              uint16_t color)
{
    int origin_x = STATUS_LVGL_WIFI_ORIGIN_OFFSET_X;
    int origin_y = STATUS_LVGL_WIFI_ORIGIN_OFFSET_Y;
    int inner_radius_squared = inner_radius * inner_radius;
    int outer_radius_squared = outer_radius * outer_radius;

    for (int local_y = 0; local_y < STATUS_LVGL_ICON_SIDE; ++local_y) {
        for (int local_x = 0; local_x < STATUS_LVGL_ICON_SIDE; ++local_x) {
            int dx = local_x - STATUS_LVGL_WIFI_ORIGIN_OFFSET_X;
            int dy = STATUS_LVGL_WIFI_ORIGIN_OFFSET_Y - local_y;
            int distance_squared = 0;

            if (dy < 0) {
                continue;
            }
            if (status_lvgl_abs_int(dx) > dy) {
                continue;
            }

            distance_squared = (dx * dx) + (dy * dy);
            if ((distance_squared >= inner_radius_squared) &&
                (distance_squared <= outer_radius_squared)) {
                if ((dx == 0) && (dy == outer_radius)) {
                    continue;
                }
                status_lvgl_set_pixel(canvas, origin_x + dx, origin_y - dy, color);
            }
        }
    }
}

static uint8_t status_lvgl_get_wifi_band_count(const display_wifi_status_icon_t *icon)
{
    if (icon->level == 0U) {
        return STATUS_LVGL_WIFI_BAND_COUNT;
    }
    if (icon->level > STATUS_LVGL_WIFI_BAND_COUNT) {
        return STATUS_LVGL_WIFI_BAND_COUNT;
    }

    return icon->level;
}

static void status_lvgl_draw_thick_line(display_canvas_t *canvas,
                                        int x0,
                                        int y0,
                                        int x1,
                                        int y1,
                                        int thickness,
                                        uint16_t color)
{
    int delta_x = status_lvgl_abs_int(x1 - x0);
    int delta_y = status_lvgl_abs_int(y1 - y0);
    int step_x = x0 < x1 ? 1 : -1;
    int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x - delta_y;
    int half_thickness = thickness / 2;

    while (true) {
        status_lvgl_fill_rect(canvas,
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

static void status_lvgl_draw_wifi_icon(const display_wifi_status_icon_t *icon, display_canvas_t *canvas)
{
    uint8_t band_count = status_lvgl_get_wifi_band_count(icon);

    status_lvgl_draw_filled_circle(canvas,
                                   STATUS_LVGL_WIFI_ORIGIN_OFFSET_X,
                                   STATUS_LVGL_WIFI_ORIGIN_OFFSET_Y,
                                   STATUS_LVGL_WIFI_DOT_RADIUS,
                                   STATUS_LVGL_COLOR_BLACK);

    for (int band_index = 0; band_index < band_count; ++band_index) {
        int outer_radius = STATUS_LVGL_WIFI_FIRST_OUTER_RADIUS +
                           (band_index * STATUS_LVGL_WIFI_RADIUS_STEP);
        int inner_radius = outer_radius - STATUS_LVGL_WIFI_BAND_THICKNESS;
        status_lvgl_draw_wifi_sector_band(canvas,
                                          inner_radius,
                                          outer_radius,
                                          STATUS_LVGL_COLOR_BLACK);
    }

    if (icon->variant == DISPLAY_STATUS_ICON_VARIANT_ALERT) {
        int slash_start_x = STATUS_LVGL_WIFI_ORIGIN_OFFSET_X +
                            STATUS_LVGL_WIFI_FIRST_OUTER_RADIUS +
                            ((STATUS_LVGL_WIFI_BAND_COUNT - 1) * STATUS_LVGL_WIFI_RADIUS_STEP) +
                            STATUS_LVGL_WIFI_SLASH_OFFSET_X;
        int slash_start_y = STATUS_LVGL_WIFI_ORIGIN_OFFSET_Y -
                            (STATUS_LVGL_WIFI_FIRST_OUTER_RADIUS +
                             ((STATUS_LVGL_WIFI_BAND_COUNT - 1) * STATUS_LVGL_WIFI_RADIUS_STEP)) + 1;
        int slash_end_x = STATUS_LVGL_WIFI_ORIGIN_OFFSET_X + STATUS_LVGL_WIFI_SLASH_OFFSET_X;
        int slash_end_y = STATUS_LVGL_WIFI_ORIGIN_OFFSET_Y + 1;

        status_lvgl_draw_thick_line(canvas,
                                    slash_start_x,
                                    slash_start_y,
                                    slash_end_x,
                                    slash_end_y,
                                    STATUS_LVGL_WIFI_SLASH_THICKNESS,
                                    STATUS_LVGL_COLOR_RED);
    }
}

void status_lvgl_image_update(lv_obj_t *img, const display_wifi_status_icon_t *icon)
{
    display_canvas_t canvas = {
        .pixels = s_pixels,
        .width = STATUS_LVGL_ICON_SIDE,
        .height = STATUS_LVGL_ICON_SIDE,
    };

    if ((img == NULL) || (icon == NULL)) {
        return;
    }

    if (!icon->visible || (icon->kind == DISPLAY_STATUS_ICON_KIND_NONE)) {
        if (!s_cached_valid || s_cached_icon.visible) {
            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            s_cached_icon = *icon;
            s_cached_valid = true;
        }
        return;
    }

    if (s_cached_valid && status_lvgl_icon_equals(&s_cached_icon, icon)) {
        return;
    }

    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    status_lvgl_image_init_dsc();
    status_lvgl_fill_pixels_parent_bg(img);
    status_lvgl_draw_wifi_icon(icon, &canvas);

    s_img_dsc.data = (const uint8_t *)s_pixels;
    s_img_dsc.data_size = (uint32_t)STATUS_LVGL_ICON_BYTES;
    lv_image_set_src(img, &s_img_dsc);
    lv_obj_invalidate(img);

    s_cached_icon = *icon;
    s_cached_valid = true;
}
