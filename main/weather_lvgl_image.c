#include "weather_lvgl_image.h"

#include "display_canvas.h"
#include "display_view_model.h"
#include "display_weather_icon_renderer.h"
#include "esp_err.h"
#include "weather_mock_provider.h"
#include "weather_presenter.h"

extern const lv_image_dsc_t weather_clear_day;

/** 与 weather_clear_day_64.c 中尺寸一致。 */
#define WEATHER_CLEAR_DAY_SIDE 64
#define WEATHER_CLEAR_DAY_PIXELS (WEATHER_CLEAR_DAY_SIDE * WEATHER_CLEAR_DAY_SIDE)

/** LVGL 9: 256 = 100% size, 512 = 200% (2x). */
#define WEATHER_LVGL_IMAGE_ZOOM_2X 512

/** 64px 位图原生 1:1 显示（与程序绘制 40px+2x 视觉接近时可改为 320 等）。 */
#define WEATHER_LVGL_BITMAP_SCALE_1X 256

static uint16_t s_clear_day_pixels[WEATHER_CLEAR_DAY_PIXELS];
static lv_image_dsc_t s_clear_day_dsc;
static int s_clear_day_dsc_ready;

/** 将位图中 RGB565 纯黑 (0x0000) 替换为父对象背景色（与透明效果类似，无需 ARGB 资源）。 */
static void weather_lvgl_prepare_clear_day_bitmap(lv_obj_t *img)
{
    size_t i;

    if (s_clear_day_dsc_ready) {
        return;
    }

    lv_obj_t *parent = lv_obj_get_parent(img);
    lv_color_t bg = lv_color_black();
    if (parent != NULL) {
        bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
    }
    {
        uint16_t bg_u16 = lv_color_to_u16(bg);
        const uint16_t *src = (const uint16_t *)weather_clear_day.data;

        for (i = 0; i < WEATHER_CLEAR_DAY_PIXELS; i++) {
            uint16_t p = src[i];
            s_clear_day_pixels[i] = (p == 0x0000U) ? bg_u16 : p;
        }
    }

    s_clear_day_dsc = weather_clear_day;
    s_clear_day_dsc.data = (const uint8_t *)s_clear_day_pixels;
    s_clear_day_dsc.data_size = (uint32_t)(WEATHER_CLEAR_DAY_PIXELS * sizeof(uint16_t));
    s_clear_day_dsc_ready = 1;
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

void weather_lvgl_image_update(lv_obj_t *img)
{
    weather_snapshot_t snap;
    display_weather_panel_t panel;
    display_canvas_t canvas;

    if (img == NULL) {
        return;
    }

    if (weather_mock_provider_get_snapshot(&snap) != ESP_OK) {
        return;
    }
    if (weather_presenter_build_panel_model(&snap, &panel) != ESP_OK) {
        return;
    }

    if (!panel.visible) {
        if (s_cached_visible != 0) {
            lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
            s_cached_visible = 0;
        }
        return;
    }

    lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);

    if ((s_cached_visible == 1) && (panel.icon == s_cached_icon)) {
        return;
    }

    if (panel.icon == WEATHER_ICON_CLEAR_DAY) {
        weather_lvgl_prepare_clear_day_bitmap(img);
        lv_image_set_src(img, &s_clear_day_dsc);
        lv_image_set_scale(img, WEATHER_LVGL_BITMAP_SCALE_1X);
        lv_obj_invalidate(img);
        s_cached_icon = panel.icon;
        s_cached_visible = 1;
        return;
    }

    weather_lvgl_image_init_dsc();

    weather_lvgl_fill_pixels_parent_bg(img);
    canvas.pixels = s_pixels;
    canvas.width = DISPLAY_WEATHER_ICON_RENDER_SIZE;
    canvas.height = DISPLAY_WEATHER_ICON_RENDER_SIZE;
    display_weather_icon_renderer_draw(&panel, &canvas, 0, 0);

    s_img_dsc.data = (const uint8_t *)s_pixels;
    s_img_dsc.data_size = (uint32_t)WEATHER_LVGL_ICON_BYTES;

    lv_image_set_src(img, &s_img_dsc);
    lv_image_set_scale(img, WEATHER_LVGL_IMAGE_ZOOM_2X);
    lv_obj_invalidate(img);

    s_cached_icon = panel.icon;
    s_cached_visible = 1;
}
