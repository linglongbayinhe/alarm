#include "weather_lvgl_image.h"

#include "display_canvas.h"
#include "display_view_model.h"
#include "display_weather_icon_renderer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern const lv_image_dsc_t weather_clear_day;
extern const lv_image_dsc_t weather_clear_night_64;

/** Uploaded weather bitmap assets are currently 64x64 RGB565 images. */
#define WEATHER_BITMAP_SIDE 64
#define WEATHER_BITMAP_PIXELS (WEATHER_BITMAP_SIDE * WEATHER_BITMAP_SIDE)

/** LVGL 9: 256 = 100% size, 512 = 200% (2x). */
#define WEATHER_LVGL_IMAGE_ZOOM_2X 512

/** Uploaded 64px bitmaps are shown 1:1 for the LVGL production UI. */
#define WEATHER_LVGL_BITMAP_SCALE_1X 256

typedef struct {
    weather_icon_kind_t icon;
    const lv_image_dsc_t *source;
    uint16_t pixels[WEATHER_BITMAP_PIXELS];
    lv_image_dsc_t descriptor;
    bool ready;
} weather_lvgl_bitmap_icon_t;

static weather_lvgl_bitmap_icon_t s_bitmap_icons[] = {
    {
        .icon = WEATHER_ICON_CLEAR_DAY,
        .source = &weather_clear_day,
    },
    {
        .icon = WEATHER_ICON_CLEAR_NIGHT,
        .source = &weather_clear_night_64,
    },
};

static weather_lvgl_bitmap_icon_t *weather_lvgl_find_bitmap_icon(weather_icon_kind_t icon)
{
    size_t index = 0;

    for (index = 0; index < (sizeof(s_bitmap_icons) / sizeof(s_bitmap_icons[0])); ++index) {
        if (s_bitmap_icons[index].icon == icon) {
            return &s_bitmap_icons[index];
        }
    }

    return NULL;
}

static bool weather_lvgl_bitmap_icon_is_supported(const weather_lvgl_bitmap_icon_t *bitmap_icon)
{
    if ((bitmap_icon == NULL) || (bitmap_icon->source == NULL) || (bitmap_icon->source->data == NULL)) {
        return false;
    }

    return (bitmap_icon->source->header.cf == LV_COLOR_FORMAT_RGB565) &&
           (bitmap_icon->source->header.w == WEATHER_BITMAP_SIDE) &&
           (bitmap_icon->source->header.h == WEATHER_BITMAP_SIDE) &&
           (bitmap_icon->source->data_size >= (uint32_t)(WEATHER_BITMAP_PIXELS * sizeof(uint16_t)));
}

/** Replace RGB565 pure black with the parent background to emulate transparency. */
static bool weather_lvgl_prepare_bitmap_icon(lv_obj_t *img, weather_lvgl_bitmap_icon_t *bitmap_icon)
{
    size_t i;
    lv_obj_t *parent = lv_obj_get_parent(img);
    lv_color_t bg = lv_color_black();

    if (bitmap_icon == NULL) {
        return false;
    }

    if (bitmap_icon->ready) {
        return true;
    }

    if (!weather_lvgl_bitmap_icon_is_supported(bitmap_icon)) {
        return false;
    }

    if (parent != NULL) {
        bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
    }
    {
        uint16_t bg_u16 = lv_color_to_u16(bg);
        const uint16_t *src = (const uint16_t *)bitmap_icon->source->data;

        for (i = 0; i < WEATHER_BITMAP_PIXELS; i++) {
            uint16_t p = src[i];
            bitmap_icon->pixels[i] = (p == 0x0000U) ? bg_u16 : p;
        }
    }

    bitmap_icon->descriptor = *bitmap_icon->source;
    bitmap_icon->descriptor.data = (const uint8_t *)bitmap_icon->pixels;
    bitmap_icon->descriptor.data_size = (uint32_t)(WEATHER_BITMAP_PIXELS * sizeof(uint16_t));
    bitmap_icon->ready = true;

    return true;
}

#define WEATHER_LVGL_ICON_PIXELS (DISPLAY_WEATHER_ICON_RENDER_SIZE * DISPLAY_WEATHER_ICON_RENDER_SIZE)
#define WEATHER_LVGL_ICON_BYTES  (WEATHER_LVGL_ICON_PIXELS * (int)sizeof(uint16_t))

static uint16_t s_pixels[WEATHER_LVGL_ICON_PIXELS];
static lv_image_dsc_t s_img_dsc;
static int s_inited;
static int s_cached_visible = -1;
static weather_icon_kind_t s_cached_icon = WEATHER_ICON_UNKNOWN;

static void weather_lvgl_image_init_dsc(void)
{
    if (s_inited) {
        return;
    }
    s_img_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_img_dsc.header.w = DISPLAY_WEATHER_ICON_RENDER_SIZE;
    s_img_dsc.header.h = DISPLAY_WEATHER_ICON_RENDER_SIZE;
    s_img_dsc.header.stride = 0;
    s_img_dsc.data = (const uint8_t *)s_pixels;
    s_img_dsc.data_size = (uint32_t)WEATHER_LVGL_ICON_BYTES;
    s_inited = 1;
}

static void weather_lvgl_fill_pixels_parent_bg(lv_obj_t *img)
{
    lv_obj_t *parent = lv_obj_get_parent(img);
    lv_color_t bg = lv_color_black();
    uint16_t fill_u16;
    int i;

    if (parent != NULL) {
        bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
    }
    fill_u16 = lv_color_to_u16(bg);
    for (i = 0; i < WEATHER_LVGL_ICON_PIXELS; i++) {
        s_pixels[i] = fill_u16;
    }
}

static bool weather_lvgl_image_try_set_bitmap_icon(lv_obj_t *img, weather_icon_kind_t icon)
{
    weather_lvgl_bitmap_icon_t *bitmap_icon = weather_lvgl_find_bitmap_icon(icon);

    if (bitmap_icon == NULL) {
        return false;
    }

    if (!weather_lvgl_prepare_bitmap_icon(img, bitmap_icon)) {
        return false;
    }

    lv_image_set_src(img, &bitmap_icon->descriptor);
    lv_image_set_scale(img, WEATHER_LVGL_BITMAP_SCALE_1X);
    lv_obj_invalidate(img);

    return true;
}

void weather_lvgl_image_update(lv_obj_t *img, const display_weather_panel_t *panel)
{
    display_canvas_t canvas;

    if ((img == NULL) || (panel == NULL)) {
        return;
    }

    if (!panel->visible) {
        if (s_cached_visible != 0) {
            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            s_cached_visible = 0;
        }
        return;
    }

    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);

    if ((s_cached_visible == 1) && (panel->icon == s_cached_icon)) {
        return;
    }

    if (weather_lvgl_image_try_set_bitmap_icon(img, panel->icon)) {
        s_cached_icon = panel->icon;
        s_cached_visible = 1;
        return;
    }

    weather_lvgl_image_init_dsc();

    weather_lvgl_fill_pixels_parent_bg(img);
    canvas.pixels = s_pixels;
    canvas.width = DISPLAY_WEATHER_ICON_RENDER_SIZE;
    canvas.height = DISPLAY_WEATHER_ICON_RENDER_SIZE;
    display_weather_icon_renderer_draw(panel, &canvas, 0, 0);

    s_img_dsc.data = (const uint8_t *)s_pixels;
    s_img_dsc.data_size = (uint32_t)WEATHER_LVGL_ICON_BYTES;

    lv_image_set_src(img, &s_img_dsc);
    lv_image_set_scale(img, WEATHER_LVGL_IMAGE_ZOOM_2X);
    lv_obj_invalidate(img);

    s_cached_icon = panel->icon;
    s_cached_visible = 1;
}
