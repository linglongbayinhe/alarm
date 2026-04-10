#include "display_service.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "display_config.h"
#include "esp_heap_caps.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "DISPLAY_SERVICE";
static const char *DATE_PLACEHOLDER = "---- -- --";
static const char *TIME_PLACEHOLDER = "--:--:--";
static const char *DISPLAY_SPIFFS_PARTITION_LABEL = NULL;

#define DISPLAY_CMD_BITS 8
#define DISPLAY_PARAM_BITS 8
#define DISPLAY_LINE_COUNT 3
#define DISPLAY_LINE_HEIGHT 64
#define DISPLAY_LINE_BUFFER_PIXELS (DISPLAY_WIDTH * DISPLAY_LINE_HEIGHT)
#define DISPLAY_DATE_SCALE 4
#define DISPLAY_TIME_SCALE 5
#define DISPLAY_DMA_WAIT_TIMEOUT_MS 1000
#define DISPLAY_COLOR_WHITE 0xFFFF
#define DISPLAY_COLOR_BLACK 0x0000
#define DISPLAY_COLOR_RED 0xF800
#define DISPLAY_STATUS_TEXT_BUFFER_SIZE 16
#define DISPLAY_WIFI_ICON_SIZE 34
#define DISPLAY_WIFI_ICON_MARGIN_RIGHT 10
#define DISPLAY_WIFI_ICON_X (DISPLAY_WIDTH - DISPLAY_WIFI_ICON_SIZE - DISPLAY_WIFI_ICON_MARGIN_RIGHT)
#define DISPLAY_WIFI_ICON_Y ((DISPLAY_LINE_HEIGHT - DISPLAY_WIFI_ICON_SIZE) / 2)
/* Wi-Fi icon drawing origin X inside the icon box. Larger moves the fan right. */
#define DISPLAY_WIFI_ICON_ORIGIN_OFFSET_X 16
/* Wi-Fi icon drawing origin Y inside the icon box. Larger moves the fan downward. */
#define DISPLAY_WIFI_ICON_ORIGIN_OFFSET_Y 28
/* Total number of Wi-Fi signal bands used by the icon. */
#define DISPLAY_WIFI_ICON_BAND_COUNT 3
/* Outer radius of the innermost fan band. Larger makes the whole icon larger. */
#define DISPLAY_WIFI_ICON_FIRST_OUTER_RADIUS 7
/* Radius increment between adjacent bands. Larger increases spacing and overall spread. */
#define DISPLAY_WIFI_ICON_RADIUS_STEP 4
/* Thickness of each white Wi-Fi fan band. Larger makes each band bolder. */
#define DISPLAY_WIFI_ICON_BAND_THICKNESS 2
#define DISPLAY_WIFI_ICON_DOT_RADIUS 2
#define DISPLAY_WIFI_ICON_SLASH_THICKNESS 3
#define DISPLAY_WIFI_ICON_SLASH_OFFSET_X -6

typedef struct {
    char ascii;
    uint8_t width;
    uint8_t rows[7];
} display_glyph_t;

static const display_glyph_t DISPLAY_GLYPHS[] = {
    {' ', 3, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', 5, {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00}},
    {':', 1, {0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0x00}},
    {'0', 5, {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', 5, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', 5, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', 5, {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
    {'4', 5, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', 5, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', 5, {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', 5, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', 5, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', 5, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'A', 5, {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'C', 5, {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'F', 5, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'I', 3, {0x07, 0x02, 0x02, 0x02, 0x02, 0x02, 0x07}},
    {'L', 5, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'O', 5, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'T', 5, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'W', 5, {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'f', 5, {0x06, 0x08, 0x08, 0x1E, 0x08, 0x08, 0x08}},
    {'i', 3, {0x02, 0x00, 0x06, 0x02, 0x02, 0x02, 0x07}},
    {'w', 5, {0x00, 0x00, 0x11, 0x15, 0x15, 0x15, 0x0A}},
};

static const int DISPLAY_LINE_START_Y[DISPLAY_LINE_COUNT] = {12, 88, 164};

static bool s_initialized;
static bool s_use_log_backend;
static bool s_backend_warning_logged;
static bool s_has_cached_view;
static uint16_t *s_line_buffer;
static SemaphoreHandle_t s_flush_done_sem;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static bool s_last_wifi_icon_visible;
static bool s_last_wifi_connected;
static uint8_t s_last_wifi_signal_level;
static bool s_last_time_valid;
static struct tm s_last_time;

/* Releases the shared line buffer once the asynchronous SPI color transfer completes. */
static bool display_flush_ready_from_isr(esp_lcd_panel_io_handle_t panel_io,
                                         esp_lcd_panel_io_event_data_t *edata,
                                         void *user_ctx)
{
    BaseType_t task_woken = pdFALSE;
    SemaphoreHandle_t flush_done_sem = (SemaphoreHandle_t)user_ctx;

    (void)panel_io;
    (void)edata;

    if (flush_done_sem != NULL) {
        xSemaphoreGiveFromISR(flush_done_sem, &task_woken);
    }

    return task_woken == pdTRUE;
}

/* Maps a supported character to the built-in bitmap glyph table. */
static const display_glyph_t *display_find_glyph(char ascii)
{
    size_t index = 0;

    for (index = 0; index < (sizeof(DISPLAY_GLYPHS) / sizeof(DISPLAY_GLYPHS[0])); ++index) {
        if (DISPLAY_GLYPHS[index].ascii == ascii) {
            return &DISPLAY_GLYPHS[index];
        }
    }

    return &DISPLAY_GLYPHS[0];
}

static esp_err_t display_push_line_buffer(int line_index);

/* Queues one LCD transfer and blocks until the driver signals that DMA has finished. */
static esp_err_t display_draw_bitmap_sync(int x_start, int y_start, int x_end, int y_end)
{
    esp_err_t ret = ESP_OK;

    if (s_flush_done_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_flush_done_sem, 0);

    ret = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, s_line_buffer);
    if (ret != ESP_OK) {
        return ret;
    }

    if (xSemaphoreTake(s_flush_done_sem, pdMS_TO_TICKS(DISPLAY_DMA_WAIT_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timed out waiting for LCD DMA flush");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

/* Copies one formatted line into a fixed-size destination buffer. */
static void display_copy_line(char *destination, size_t destination_size, const char *source)
{
    snprintf(destination, destination_size, "%s", source);
}

/* Forces the next UI refresh to redraw every region even if the view model did not change. */
static void display_invalidate_cached_view(void)
{
    s_has_cached_view = false;
}

/* Compares the displayed clock fields we actually render on the screen. */
static bool display_time_equals(const struct tm *left, const struct tm *right)
{
    return (left->tm_year == right->tm_year) &&
           (left->tm_mon == right->tm_mon) &&
           (left->tm_mday == right->tm_mday) &&
           (left->tm_hour == right->tm_hour) &&
           (left->tm_min == right->tm_min) &&
           (left->tm_sec == right->tm_sec);
}

/* Formats a textual representation of the Wi-Fi icon state for log fallback. */
static void display_format_status_line(const display_view_model_t *view_model,
                                       char *status_line,
                                       size_t status_line_size)
{
    if (!view_model->wifi_icon_visible) {
        display_copy_line(status_line, status_line_size, "ICON HIDDEN");
    } else if (!view_model->wifi_connected) {
        display_copy_line(status_line, status_line_size, "WIFI OFF");
    } else if (view_model->wifi_signal_level == 0U) {
        display_copy_line(status_line, status_line_size, "WIFI BASE");
    } else {
        snprintf(status_line, status_line_size, "WIFI L%u", view_model->wifi_signal_level);
    }
}

/* Formats the date and time lines for the renderer or log fallback. */
static void display_format_time_lines(const display_view_model_t *view_model,
                                      char *date_line,
                                      size_t date_line_size,
                                      char *time_line,
                                      size_t time_line_size)
{
    if (view_model->time_valid) {
        strftime(date_line, date_line_size, "%Y-%m-%d", &view_model->current_time);
        strftime(time_line, time_line_size, "%H:%M:%S", &view_model->current_time);
    } else {
        display_copy_line(date_line, date_line_size, DATE_PLACEHOLDER);
        display_copy_line(time_line, time_line_size, TIME_PLACEHOLDER);
    }
}

/* Clears the reusable line buffer before rendering a new strip. */
static void display_buffer_fill(uint16_t color)
{
    size_t index = 0;

    for (index = 0; index < DISPLAY_LINE_BUFFER_PIXELS; ++index) {
        s_line_buffer[index] = color;
    }
}

/* Writes one pixel into the temporary line buffer with bounds checking. */
static void display_buffer_set_pixel(int x, int y, uint16_t color)
{
    if ((x < 0) || (x >= DISPLAY_WIDTH) || (y < 0) || (y >= DISPLAY_LINE_HEIGHT)) {
        return;
    }

    s_line_buffer[(y * DISPLAY_WIDTH) + x] = color;
}

/* Draws a filled rectangle into the temporary line buffer. */
static void display_buffer_fill_rect(int x, int y, int width, int height, uint16_t color)
{
    int x_end = x + width;
    int y_end = y + height;
    int current_x = 0;
    int current_y = 0;

    for (current_y = y; current_y < y_end; ++current_y) {
        for (current_x = x; current_x < x_end; ++current_x) {
            display_buffer_set_pixel(current_x, current_y, color);
        }
    }
}

/* Measures the rendered width of an ASCII text line at the requested scale. */
static int display_measure_ascii_text(const char *text, int scale)
{
    int width = 0;
    bool first = true;

    while ((text != NULL) && (*text != '\0')) {
        const display_glyph_t *glyph = display_find_glyph(*text);
        if (!first) {
            width += scale;
        }
        width += glyph->width * scale;
        first = false;
        ++text;
    }

    return width;
}

/* Draws one ASCII text line into the temporary line buffer. */
static void display_draw_ascii_text(const char *text, int scale, int x_offset, int y_offset, uint16_t color)
{
    bool first = true;

    while ((text != NULL) && (*text != '\0')) {
        const display_glyph_t *glyph = display_find_glyph(*text);
        int row = 0;
        int column = 0;

        if (!first) {
            x_offset += scale;
        }

        for (row = 0; row < 7; ++row) {
            uint8_t row_bits = glyph->rows[row];
            for (column = 0; column < glyph->width; ++column) {
                int bit_index = glyph->width - 1 - column;
                if ((row_bits & (1U << bit_index)) != 0U) {
                    display_buffer_fill_rect(x_offset + (column * scale),
                                             y_offset + (row * scale),
                                             scale,
                                             scale,
                                             color);
                }
            }
        }

        x_offset += glyph->width * scale;
        first = false;
        ++text;
    }
}

/* Returns the absolute value of a signed integer. */
static int display_abs_int(int value)
{
    return value < 0 ? -value : value;
}

/* Draws a filled circle into the temporary line buffer. */
static void display_draw_filled_circle(int center_x, int center_y, int radius, uint16_t color)
{
    int row = 0;
    int column = 0;
    int radius_squared = radius * radius;

    for (row = -radius; row <= radius; ++row) {
        for (column = -radius; column <= radius; ++column) {
            int distance_squared = (column * column) + (row * row);

            if (distance_squared <= radius_squared) {
                display_buffer_set_pixel(center_x + column, center_y + row, color);
            }
        }
    }
}

/* Draws one 90-degree fan band centered on the upward vertical axis. */
static void display_draw_wifi_sector_band(int x_offset, int y_offset, int inner_radius, int outer_radius, uint16_t color)
{
    int local_x = 0;
    int local_y = 0;
    int origin_x = x_offset + DISPLAY_WIFI_ICON_ORIGIN_OFFSET_X;
    int origin_y = y_offset + DISPLAY_WIFI_ICON_ORIGIN_OFFSET_Y;
    int inner_radius_squared = inner_radius * inner_radius;
    int outer_radius_squared = outer_radius * outer_radius;

    for (local_y = 0; local_y < DISPLAY_WIFI_ICON_SIZE; ++local_y) {
        for (local_x = 0; local_x < DISPLAY_WIFI_ICON_SIZE; ++local_x) {
            int dx = local_x - DISPLAY_WIFI_ICON_ORIGIN_OFFSET_X;
            int dy = DISPLAY_WIFI_ICON_ORIGIN_OFFSET_Y - local_y;
            int distance_squared = 0;

            if (dy < 0) {
                continue;
            }
            if (display_abs_int(dx) > dy) {
                continue;
            }

            distance_squared = (dx * dx) + (dy * dy);
            if ((distance_squared >= inner_radius_squared) &&
                (distance_squared <= outer_radius_squared)) {
                display_buffer_set_pixel(origin_x + dx, origin_y - dy, color);
            }
        }
    }
}

/* Draws a thick diagonal slash used by the disconnected icon state. */
static void display_draw_thick_line(int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    int delta_x = display_abs_int(x1 - x0);
    int delta_y = display_abs_int(y1 - y0);
    int step_x = x0 < x1 ? 1 : -1;
    int step_y = y0 < y1 ? 1 : -1;
    int error = delta_x - delta_y;
    int half_thickness = thickness / 2;

    while (true) {
        display_buffer_fill_rect(x0 - half_thickness,
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

/* Resolves how many white Wi-Fi bands should be shown for the current signal state. */
static uint8_t display_get_wifi_band_count(const display_view_model_t *view_model)
{
    if (!view_model->wifi_connected) {
        return DISPLAY_WIFI_ICON_BAND_COUNT;
    }
    if ((view_model->wifi_signal_level < 1U) || (view_model->wifi_signal_level > DISPLAY_WIFI_ICON_BAND_COUNT)) {
        return DISPLAY_WIFI_ICON_BAND_COUNT;
    }

    return view_model->wifi_signal_level;
}

/* Draws the base Wi-Fi fan icon at the fixed top-right location. */
static void display_draw_wifi_icon_base(const display_view_model_t *view_model)
{
    uint8_t band_count = display_get_wifi_band_count(view_model);
    int band_index = 0;
    int outer_radius = 0;
    int inner_radius = 0;
    int dot_center_x = DISPLAY_WIFI_ICON_X + DISPLAY_WIFI_ICON_ORIGIN_OFFSET_X;
    int dot_center_y = DISPLAY_WIFI_ICON_Y + DISPLAY_WIFI_ICON_ORIGIN_OFFSET_Y;

    display_draw_filled_circle(dot_center_x, dot_center_y, DISPLAY_WIFI_ICON_DOT_RADIUS, DISPLAY_COLOR_WHITE);

    for (band_index = 0; band_index < band_count; ++band_index) {
        outer_radius = DISPLAY_WIFI_ICON_FIRST_OUTER_RADIUS + (band_index * DISPLAY_WIFI_ICON_RADIUS_STEP);
        inner_radius = outer_radius - DISPLAY_WIFI_ICON_BAND_THICKNESS;
        display_draw_wifi_sector_band(DISPLAY_WIFI_ICON_X,
                                      DISPLAY_WIFI_ICON_Y,
                                      inner_radius,
                                      outer_radius,
                                      DISPLAY_COLOR_WHITE);
    }
}

/* Overlays the red slash used by the disconnected Wi-Fi icon state. */
static void display_draw_wifi_icon_disconnected_slash(void)
{
    int slash_start_x = DISPLAY_WIFI_ICON_X + DISPLAY_WIFI_ICON_ORIGIN_OFFSET_X + DISPLAY_WIFI_ICON_FIRST_OUTER_RADIUS +
                        ((DISPLAY_WIFI_ICON_BAND_COUNT - 1) * DISPLAY_WIFI_ICON_RADIUS_STEP) DISPLAY_WIFI_ICON_SLASH_OFFSET_X;
    int slash_start_y = DISPLAY_WIFI_ICON_Y + DISPLAY_WIFI_ICON_ORIGIN_OFFSET_Y -
                        (DISPLAY_WIFI_ICON_FIRST_OUTER_RADIUS +
                         ((DISPLAY_WIFI_ICON_BAND_COUNT - 1) * DISPLAY_WIFI_ICON_RADIUS_STEP)) + 1;
    int slash_end_x = DISPLAY_WIFI_ICON_X + DISPLAY_WIFI_ICON_ORIGIN_OFFSET_X DISPLAY_WIFI_ICON_SLASH_OFFSET_X;
    int slash_end_y = DISPLAY_WIFI_ICON_Y + DISPLAY_WIFI_ICON_ORIGIN_OFFSET_Y + 1;

    display_draw_thick_line(slash_start_x,
                            slash_start_y,
                            slash_end_x,
                            slash_end_y,
                            DISPLAY_WIFI_ICON_SLASH_THICKNESS,
                            DISPLAY_COLOR_RED);
}

/* Clears the top status region and draws the Wi-Fi icon at the fixed top-right location. */
static esp_err_t display_render_status_icon_region(const display_view_model_t *view_model)
{
    display_buffer_fill(DISPLAY_COLOR_BLACK);

    if (view_model->wifi_icon_visible) {
        display_draw_wifi_icon_base(view_model);
        if (!view_model->wifi_connected) {
            display_draw_wifi_icon_disconnected_slash();
        }
    }

    return display_push_line_buffer(0);
}



/* Pushes the current line buffer to the corresponding vertical region on the LCD. */
static esp_err_t display_push_line_buffer(int line_index)
{
    int y_start = DISPLAY_LINE_START_Y[line_index];

    return display_draw_bitmap_sync(0,
                                    y_start,
                                    DISPLAY_WIDTH,
                                    y_start + DISPLAY_LINE_HEIGHT);
}

/* Renders one centered line of text into its screen region. */
static esp_err_t display_render_line_region(int line_index, const char *text, int scale)
{
    int text_width = 0;
    int x_offset = 0;
    int y_offset = 0;

    display_buffer_fill(DISPLAY_COLOR_BLACK);

    text_width = display_measure_ascii_text(text, scale);
    x_offset = (DISPLAY_WIDTH - text_width) / 2;
    y_offset = (DISPLAY_LINE_HEIGHT - (7 * scale)) / 2;
    display_draw_ascii_text(text, scale, x_offset, y_offset, DISPLAY_COLOR_WHITE);

    return display_push_line_buffer(line_index);
}

/* Clears all line regions during display startup. */
static esp_err_t display_clear_screen(void)
{
    int y_start = 0;
    int segment_height = 0;
    esp_err_t ret = ESP_OK;

    display_buffer_fill(DISPLAY_COLOR_BLACK);
    for (y_start = 0; y_start < DISPLAY_HEIGHT; y_start += DISPLAY_LINE_HEIGHT) {
        segment_height = DISPLAY_HEIGHT - y_start;
        if (segment_height > DISPLAY_LINE_HEIGHT) {
            segment_height = DISPLAY_LINE_HEIGHT;
        }

        ret = display_draw_bitmap_sync(0,
                                       y_start,
                                       DISPLAY_WIDTH,
                                       y_start + segment_height);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

/* Configures SPI, panel IO and ST7789 using the project-local wiring. */
static esp_err_t display_init_hardware(void)
{
    esp_err_t ret = ESP_OK;
    esp_lcd_panel_io_callbacks_t panel_io_callbacks = {
        .on_color_trans_done = display_flush_ready_from_isr,
    };
    spi_bus_config_t bus_config = {
        .sclk_io_num = DISPLAY_PIN_SCLK,
        .mosi_io_num = DISPLAY_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_LINE_BUFFER_PIXELS * sizeof(uint16_t),
    };
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = DISPLAY_PIN_CS,
        .dc_gpio_num = DISPLAY_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = DISPLAY_SPI_FREQUENCY_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = DISPLAY_CMD_BITS,
        .lcd_param_bits = DISPLAY_PARAM_BITS,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = DISPLAY_PIN_RST,
#if DISPLAY_RGB_ORDER_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
    };
    gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << DISPLAY_PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };

    if (s_flush_done_sem == NULL) {
        s_flush_done_sem = xSemaphoreCreateBinary();
        if (s_flush_done_sem == NULL) {
            ESP_LOGE(TAG, "Failed to create LCD flush semaphore");
            return ESP_ERR_NO_MEM;
        }
    }

    s_line_buffer = heap_caps_malloc(DISPLAY_LINE_BUFFER_PIXELS * sizeof(uint16_t),
                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_line_buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate LCD line buffer");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(gpio_config(&backlight_config), TAG, "Backlight GPIO init failed");
    gpio_set_level(DISPLAY_PIN_BACKLIGHT, !DISPLAY_BACKLIGHT_ON_LEVEL);

    ret = spi_bus_initialize(DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &s_panel_io),
                        TAG,
                        "Panel IO init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_panel_io,
                                                                  &panel_io_callbacks,
                                                                  s_flush_done_sem),
                        TAG,
                        "Panel IO callback registration failed");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel),
                        TAG,
                        "ST7789 panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel hardware init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_panel, DISPLAY_SWAP_XY), TAG, "Panel swap XY failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y),
                        TAG,
                        "Panel mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, DISPLAY_GAP_X, DISPLAY_GAP_Y),
                        TAG,
                        "Panel gap config failed");
#if DISPLAY_INVERT_COLOR
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "Panel invert failed");
#endif
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "Panel display on failed");

    ESP_RETURN_ON_ERROR(display_clear_screen(), TAG, "Panel clear failed");
    gpio_set_level(DISPLAY_PIN_BACKLIGHT, DISPLAY_BACKLIGHT_ON_LEVEL);

    ESP_LOGI(TAG, "ST7789 display initialized on project-local pin config");

    return ESP_OK;
}

/* Initializes the display backend and allocates shared render buffers. */
esp_err_t display_service_init(void)
{
    esp_err_t ret = ESP_OK;

    s_use_log_backend = false;
    s_backend_warning_logged = false;
    s_has_cached_view = false;
    s_last_wifi_icon_visible = false;
    s_last_wifi_connected = false;
    s_last_wifi_signal_level = 0;
    s_last_time_valid = false;
    memset(&s_last_time, 0, sizeof(s_last_time));

    ret = display_init_hardware();
    if (ret != ESP_OK) {
        s_use_log_backend = true;
        ESP_LOGW(TAG, "Display hardware init failed, fallback to log renderer");
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Display service initialized");

    return ESP_OK;
}

/* Renders the current three-line view model and only updates changed lines. */
esp_err_t display_service_render(const display_view_model_t *view_model)
{
    char status_line[DISPLAY_STATUS_TEXT_BUFFER_SIZE];
    char date_line[16];
    char time_line[16];
    bool time_unchanged = false;
    esp_err_t ret = ESP_OK;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (view_model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    display_format_status_line(view_model, status_line, sizeof(status_line));
    display_format_time_lines(view_model,
                              date_line,
                              sizeof(date_line),
                              time_line,
                              sizeof(time_line));

    time_unchanged = (!view_model->time_valid && !s_last_time_valid) ||
                     (view_model->time_valid &&
                      s_last_time_valid &&
                      display_time_equals(&view_model->current_time, &s_last_time));

    if (s_has_cached_view &&
        (view_model->wifi_icon_visible == s_last_wifi_icon_visible) &&
        (view_model->wifi_connected == s_last_wifi_connected) &&
        (view_model->wifi_signal_level == s_last_wifi_signal_level) &&
        (view_model->time_valid == s_last_time_valid) &&
        time_unchanged) {
        return ESP_OK;
    }

    if (s_use_log_backend) {
        if (!s_backend_warning_logged) {
            ESP_LOGW(TAG, "Using log renderer because TFT backend is unavailable");
            s_backend_warning_logged = true;
        }
        ESP_LOGI(TAG, "Screen line1: %s", status_line);
        ESP_LOGI(TAG, "Screen line2: %s", date_line);
        ESP_LOGI(TAG, "Screen line3: %s", time_line);
    } else {
        ret = display_render_status_icon_region(view_model);
        if (ret == ESP_OK) {
            ret = display_render_line_region(1, date_line, DISPLAY_DATE_SCALE);
        }
        if (ret == ESP_OK) {
            ret = display_render_line_region(2, time_line, DISPLAY_TIME_SCALE);
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD update failed: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    s_has_cached_view = true;
    s_last_wifi_icon_visible = view_model->wifi_icon_visible;
    s_last_wifi_connected = view_model->wifi_connected;
    s_last_wifi_signal_level = view_model->wifi_signal_level;
    s_last_time_valid = view_model->time_valid;
    if (view_model->time_valid) {
        s_last_time = view_model->current_time;
    } else {
        memset(&s_last_time, 0, sizeof(s_last_time));
    }

    return ESP_OK;
}

/* ============================================================================
 * 图片显示接口 - 从SPIFFS文件系统或内存读取并显示RGB565格式的图片
 * ============================================================================== */

/**
 * @brief 从SPIFFS文件系统读取并显示全屏RGB565格式的图片
 * 
 * 该函数从指定的SPIFFS文件路径读取RGB565格式的图片数据，并通过SPI显示在屏幕上。
 * RGB565格式：每个像素占用2个字节（R5位+G6位+B5位）
 * 期望的文件大小：DISPLAY_WIDTH * DISPLAY_HEIGHT * 2 字节（320*240*2 = 153,600字节）
 * 
 * 使用示例：
 *   display_service_show_image("/spiffs/logo.rgb565");
 * 
 * @param image_path SPIFFS文件完整路径，例如 "/spiffs/logo.rgb565"
 * @return 
 *   - ESP_OK: 成功显示图片
 *   - ESP_ERR_INVALID_ARG: image_path 为 NULL
 *   - ESP_ERR_INVALID_STATE: 显示服务未初始化或后端不可用
 *   - ESP_ERR_NOT_FOUND: 文件不存在
 *   - ESP_ERR_NO_MEM: DMA缓冲区内存不足（无法分配临时读取缓冲）
 *   - ESP_FAIL: 文件读取错误或数据不完整
 */
esp_err_t display_service_show_image(const char *image_path)
{
    esp_err_t ret = ESP_OK;
    FILE *file = NULL;
    size_t bytes_read = 0;
    long file_size = 0;
    size_t expected_size = (size_t)DISPLAY_WIDTH * (size_t)DISPLAY_HEIGHT * sizeof(uint16_t);
    int y_pos = 0;
    int segment_height = 0;

    /* ========== 参数检查 ========== */
    if (!s_initialized) {
        ESP_LOGE(TAG, "Display service not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (image_path == NULL || image_path[0] == '\0') {
        ESP_LOGE(TAG, "Image path is NULL or empty");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_use_log_backend) {
        ESP_LOGW(TAG, "Cannot display image: TFT backend unavailable, using log fallback");
        return ESP_ERR_INVALID_STATE;
    }

    /* ========== 打开图片文件 ========== */
    if (!esp_spiffs_mounted(DISPLAY_SPIFFS_PARTITION_LABEL)) {
        ESP_LOGE(TAG, "SPIFFS is not mounted; cannot load image file");
        return ESP_ERR_INVALID_STATE;
    }

    file = fopen(image_path, "rb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Failed to open image file: %s", image_path);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Opened image file: %s", image_path);

    if ((fseek(file, 0, SEEK_END) != 0) || ((file_size = ftell(file)) < 0) || (fseek(file, 0, SEEK_SET) != 0)) {
        ESP_LOGE(TAG, "Failed to determine image file size: %s", image_path);
        fclose(file);
        return ESP_FAIL;
    }

    if ((size_t)file_size != expected_size) {
        ESP_LOGE(TAG,
                 "Invalid image size for %s: expected %u bytes, got %ld bytes",
                 image_path,
                 (unsigned int)expected_size,
                 file_size);
        fclose(file);
        return ESP_ERR_INVALID_SIZE;
    }

    /* ========== 逐行读取并显示图片 ========== */
    /* 
     * 策略：利用现有的 s_line_buffer (DMA行缓冲)，逐行读取图片数据并显示
     * 这样避免一次性分配 DISPLAY_WIDTH * DISPLAY_HEIGHT * 2 字节的大内存
     */
    for (y_pos = 0; y_pos < DISPLAY_HEIGHT; y_pos += DISPLAY_LINE_HEIGHT) {
        /* 确定本次要读取的段高度 */
        segment_height = DISPLAY_HEIGHT - y_pos;
        if (segment_height > DISPLAY_LINE_HEIGHT) {
            segment_height = DISPLAY_LINE_HEIGHT;
        }

        /* 
         * 计算本行数据的字节数
         * RGB565格式：每像素2字节
         * 本行字节数 = 宽度 (像素) × 高度 (像素) × 2 (字节/像素)
         */
        size_t line_data_size = DISPLAY_WIDTH * segment_height * sizeof(uint16_t);

        /* 从文件读取本行数据到 s_line_buffer */
        bytes_read = fread(s_line_buffer, 1, line_data_size, file);
        if (bytes_read != line_data_size) {
            ESP_LOGE(TAG, 
                     "Image file read error at Y=%d: expected %d bytes, got %d bytes",
                     y_pos, 
                     line_data_size, 
                     bytes_read);
            fclose(file);
            return ESP_FAIL;
        }

        /* 将本行数据通过SPI DMA发送到屏幕对应区域 */
        ret = display_draw_bitmap_sync(0, y_pos, DISPLAY_WIDTH, y_pos + segment_height);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD update failed at Y=%d: %s", y_pos, esp_err_to_name(ret));
            fclose(file);
            return ret;
        }
    }

    /* ========== 清理资源 ========== */
    fclose(file);
    file = NULL;
    display_invalidate_cached_view();

    ESP_LOGI(TAG, "Image display completed successfully");
    return ESP_OK;
}

/**
 * @brief 从内存中的像素数组显示全屏图片（用于预加载在RAM或ROM中的图像数据）
 * 
 * 该函数适用于将图片数据预先存储在RAM或ROM中的场景，例如：
 * - 固件中嵌入的Logo或启动画面（存储在ROM中）
 * - 从网络/SD卡加载到RAM中后再显示
 * 
 * 使用示例：
 *   // 假设有外部定义的图片数组
 *   extern const uint16_t logo_data[];
 *   display_service_show_image_data(logo_data, 320, 240);
 * 
 * @param image_data RGB565格式的像素数据指针
 *                   数组应包含 width * height 个 uint16_t 元素
 *                   总大小应为 width * height * 2 字节
 * @param width 图片宽度，通常应为 DISPLAY_WIDTH (320)
 * @param height 图片高度，通常应为 DISPLAY_HEIGHT (240)
 * 
 * @return 
 *   - ESP_OK: 成功显示
 *   - ESP_ERR_INVALID_ARG: image_data为NULL或宽高不匹配
 *   - ESP_ERR_INVALID_STATE: 显示服务未初始化或后端不可用
 */
esp_err_t display_service_show_image_data(const uint16_t *image_data, int width, int height)
{
    esp_err_t ret = ESP_OK;
    int y_pos = 0;
    int segment_height = 0;
    size_t line_data_size = 0;
    size_t line_offset = 0;

    /* ========== 参数检查 ========== */
    if (!s_initialized) {
        ESP_LOGE(TAG, "Display service not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (image_data == NULL) {
        ESP_LOGE(TAG, "Image data pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if ((width != DISPLAY_WIDTH) || (height != DISPLAY_HEIGHT)) {
        ESP_LOGE(TAG,
                 "Image dimensions must match the display size: expected %dx%d, got %dx%d",
                 DISPLAY_WIDTH,
                 DISPLAY_HEIGHT,
                 width,
                 height);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_use_log_backend) {
        ESP_LOGW(TAG, "Cannot display image: TFT backend unavailable, using log fallback");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Displaying image from memory: %dx%d", width, height);

    /* ========== 逐行复制并显示图片 ========== */
    /* 
     * 策略：为了避免占用过多内存，逐行复制像素数据到 s_line_buffer，
     * 然后通过SPI DMA发送到屏幕
     * 如果图片尺寸与屏幕和行高度不匹配，此函数仍可工作但可能显示异常
     */
    for (y_pos = 0; y_pos < height; y_pos += DISPLAY_LINE_HEIGHT) {
        segment_height = height - y_pos;
        if (segment_height > DISPLAY_LINE_HEIGHT) {
            segment_height = DISPLAY_LINE_HEIGHT;
        }

        /* 
         * 计算本行在内存中的偏移量
         * 偏移 = y位置 × 屏幕宽度 × 字节数
         */
        line_offset = y_pos * width;
        line_data_size = width * segment_height * sizeof(uint16_t);

        /* 将本行数据复制到DMA缓冲 */
        memcpy(s_line_buffer, &image_data[line_offset], line_data_size);

        /* 通过SPI DMA发送到屏幕 */
        ret = display_draw_bitmap_sync(0, y_pos, width, y_pos + segment_height);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "LCD update failed at Y=%d: %s", y_pos, esp_err_to_name(ret));
            return ret;
        }
    }

    display_invalidate_cached_view();
    ESP_LOGI(TAG, "Image display from memory completed successfully");
    return ESP_OK;
}
