/**
 * Host tool: rasterize display_weather_icon_renderer to PNG files (same pixels as firmware).
 *
 * Build (from this directory, MinGW or GCC):
 *   gcc -O2 -I../../main/display ../../main/display/display_weather_icon_renderer.c export_weather_icons.c -o export_weather_icons
 *
 * Run:
 *   ./export_weather_icons [output_dir]
 * Default output_dir is ./out
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "display_canvas.h"
#include "display_view_model.h"
#include "display_weather_icon_renderer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

static void rgb565_to_rgb888(uint16_t p, unsigned char *out)
{
    out[0] = (unsigned char)(((p >> 11) & 0x1FU) * 255 / 31);
    out[1] = (unsigned char)(((p >> 5) & 0x3FU) * 255 / 63);
    out[2] = (unsigned char)((p & 0x1FU) * 255 / 31);
}

static const char *icon_filename(weather_icon_kind_t icon)
{
    switch (icon) {
        case WEATHER_ICON_UNKNOWN:
            return "weather_unknown";
        case WEATHER_ICON_CLEAR_DAY:
            return "weather_clear_day";
        case WEATHER_ICON_CLEAR_NIGHT:
            return "weather_clear_night";
        case WEATHER_ICON_CLOUDY_DAY:
            return "weather_cloudy_day";
        case WEATHER_ICON_CLOUDY_NIGHT:
            return "weather_cloudy_night";
        case WEATHER_ICON_OVERCAST:
            return "weather_overcast";
        case WEATHER_ICON_LIGHT_RAIN:
            return "weather_light_rain";
        case WEATHER_ICON_MODERATE_RAIN:
            return "weather_moderate_rain";
        case WEATHER_ICON_HEAVY_RAIN:
            return "weather_heavy_rain";
        case WEATHER_ICON_SHOWER:
            return "weather_shower";
        case WEATHER_ICON_THUNDERSTORM:
            return "weather_thunderstorm";
        case WEATHER_ICON_SNOW:
            return "weather_snow";
        case WEATHER_ICON_FOG:
            return "weather_fog";
        case WEATHER_ICON_HAZE:
            return "weather_haze";
        case WEATHER_ICON_DUST_STORM:
            return "weather_dust_storm";
        case WEATHER_ICON_WINDY:
            return "weather_windy";
        default:
            return "weather_unknown";
    }
}

int main(int argc, char **argv)
{
    const char *out_dir = (argc >= 2) ? argv[1] : "out";
    char path[512];
    int w = DISPLAY_WEATHER_ICON_RENDER_SIZE;
    int h = DISPLAY_WEATHER_ICON_RENDER_SIZE;
    size_t pix_count = (size_t)w * (size_t)h;
    uint16_t *rgb565 = (uint16_t *)malloc(pix_count * sizeof(uint16_t));
    unsigned char *rgb = (unsigned char *)malloc(pix_count * 3U);
    if ((rgb565 == NULL) || (rgb == NULL)) {
        fprintf(stderr, "alloc failed\n");
        return 1;
    }

#ifdef _WIN32
    if (_mkdir(out_dir) != 0) {
        /* ignore "already exists" */
    }
#else
    if (mkdir(out_dir, 0755) != 0) {
        /* ignore EEXIST */
    }
#endif

    for (int i = 0; i <= 15; ++i) {
        weather_icon_kind_t icon = (weather_icon_kind_t)i;
        display_weather_panel_t panel;
        memset(&panel, 0, sizeof(panel));
        panel.visible = true;
        panel.icon = icon;

        memset(rgb565, 0, pix_count * sizeof(uint16_t));
        display_canvas_t canvas = {
            .pixels = rgb565,
            .width = w,
            .height = h,
        };
        display_weather_icon_renderer_draw(&panel, &canvas, 0, 0);

        for (size_t j = 0; j < pix_count; ++j) {
            rgb565_to_rgb888(rgb565[j], rgb + j * 3U);
        }

        snprintf(path, sizeof(path), "%s/%s.png", out_dir, icon_filename(icon));
        if (!stbi_write_png(path, w, h, 3, rgb, w * 3)) {
            fprintf(stderr, "failed to write %s\n", path);
            free(rgb565);
            free(rgb);
            return 1;
        }
        printf("%s\n", path);
    }

    free(rgb565);
    free(rgb);
    return 0;
}
