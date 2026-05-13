#include "display_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "display_config.h"
#include "display_lvgl.h"
#include "display_lvgl_port_cfg.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

static const char *TAG = "DISPLAY_SERVICE";
static const char *DATE_PLACEHOLDER = "----.--.--";
static const char *TIME_PLACEHOLDER = "--:--:--";

#define DISPLAY_CMD_BITS 8
#define DISPLAY_PARAM_BITS 8
#define DISPLAY_STATUS_TEXT_BUFFER_SIZE 128

static bool s_initialized;
static bool s_log_fallback;
static bool s_backend_warning_logged;
static bool s_has_cached_view;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static display_wifi_status_icon_t s_last_top_right_icon;
static bool s_last_weather_visible;
static weather_icon_kind_t s_last_weather_icon;
static bool s_last_time_valid;
static struct tm s_last_time;

static void display_copy_line(char *destination, size_t destination_size, const char *source)
{
    snprintf(destination, destination_size, "%s", source);
}

static bool display_time_equals(const struct tm *left, const struct tm *right)
{
    return (left->tm_year == right->tm_year) &&
           (left->tm_mon == right->tm_mon) &&
           (left->tm_mday == right->tm_mday) &&
           (left->tm_hour == right->tm_hour) &&
           (left->tm_min == right->tm_min) &&
           (left->tm_sec == right->tm_sec);
}

static bool display_status_icon_equals(const display_wifi_status_icon_t *left,
                                       const display_wifi_status_icon_t *right)
{
    return (left->visible == right->visible) &&
           (left->kind == right->kind) &&
           (left->variant == right->variant) &&
           (left->level == right->level);
}

static void display_format_status_line(const display_wifi_status_icon_t *icon,
                                       char *status_line,
                                       size_t status_line_size)
{
    if ((icon == NULL) || !icon->visible || (icon->kind == DISPLAY_STATUS_ICON_KIND_NONE)) {
        display_copy_line(status_line, status_line_size, "ICON HIDDEN");
        return;
    }

    snprintf(status_line,
             status_line_size,
             "ICON kind=%u variant=%u level=%u",
             (unsigned int)icon->kind,
             (unsigned int)icon->variant,
             (unsigned int)icon->level);
}

static void display_format_weather_line(const display_weather_panel_t *panel,
                                        char *weather_line,
                                        size_t weather_line_size)
{
    if ((panel == NULL) || !panel->visible) {
        display_copy_line(weather_line, weather_line_size, "WEATHER HIDDEN");
        return;
    }

    snprintf(weather_line,
             weather_line_size,
             "WEATHER icon=%u text=%s temp=%s",
             (unsigned int)panel->icon,
             panel->condition_text,
             panel->temperature_text);
}

static void display_format_time_lines(const display_view_model_t *view_model,
                                      char *date_line,
                                      size_t date_line_size,
                                      char *time_line,
                                      size_t time_line_size)
{
    if (view_model->time_valid) {
        strftime(date_line, date_line_size, "%Y.%m.%d", &view_model->current_time);
        strftime(time_line, time_line_size, "%H:%M:%S", &view_model->current_time);
        return;
    }

    display_copy_line(date_line, date_line_size, DATE_PLACEHOLDER);
    display_copy_line(time_line, time_line_size, TIME_PLACEHOLDER);
}

static esp_err_t display_init_hardware(void)
{
    esp_err_t ret = ESP_OK;
    spi_bus_config_t bus_config = {
        .sclk_io_num = DISPLAY_PIN_SCLK,
        .mosi_io_num = DISPLAY_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_LVGL_PORT_BUFFER_PIXELS * sizeof(uint16_t),
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
#if DISPLAY_RGB_DATA_ENDIAN_LITTLE
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
#else
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
#endif
        .bits_per_pixel = 16,
    };
    gpio_config_t backlight_config = {
        .pin_bit_mask = 1ULL << DISPLAY_PIN_BACKLIGHT,
        .mode = GPIO_MODE_OUTPUT,
    };

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

    gpio_set_level(DISPLAY_PIN_BACKLIGHT, DISPLAY_BACKLIGHT_ON_LEVEL);

    ESP_LOGI(TAG,
             "ST7789 display initialized on project-local pin config (order=%s, invert=%d, endian=%s)",
             DISPLAY_RGB_ORDER_BGR ? "BGR" : "RGB",
             DISPLAY_INVERT_COLOR ? 1 : 0,
             DISPLAY_RGB_DATA_ENDIAN_LITTLE ? "LITTLE" : "BIG");

    return ESP_OK;
}

esp_err_t display_service_init(void)
{
    esp_err_t ret = ESP_OK;

    s_log_fallback = false;
    s_backend_warning_logged = false;
    s_has_cached_view = false;
    memset(&s_last_top_right_icon, 0, sizeof(s_last_top_right_icon));
    s_last_weather_visible = false;
    s_last_weather_icon = WEATHER_ICON_UNKNOWN;
    s_last_time_valid = false;
    memset(&s_last_time, 0, sizeof(s_last_time));

    ret = display_init_hardware();
    if (ret != ESP_OK) {
        s_log_fallback = true;
        ESP_LOGW(TAG, "Display hardware init failed; LVGL UI is unavailable");
    } else {
        esp_err_t lvgl_ret = display_lvgl_init(s_panel_io, s_panel);
        if (lvgl_ret != ESP_OK) {
            s_log_fallback = true;
            ESP_LOGW(TAG, "LVGL port init failed: %s; using log fallback", esp_err_to_name(lvgl_ret));
        }
    }

    s_initialized = true;

    ESP_LOGI(TAG, "Display service initialized");

    return ESP_OK;
}

esp_err_t display_service_render(const display_view_model_t *view_model)
{
    char status_line[DISPLAY_STATUS_TEXT_BUFFER_SIZE];
    char date_line[16];
    char time_line[16];
    char weather_line[DISPLAY_STATUS_TEXT_BUFFER_SIZE];
    bool status_icon_changed = false;
    bool time_unchanged = false;
    bool time_changed = false;
    bool weather_changed = false;

    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (view_model == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (display_lvgl_is_active()) {
        return ESP_OK;
    }

    if (!s_backend_warning_logged) {
        ESP_LOGW(TAG,
                 "%s",
                 s_log_fallback ? "Using log fallback because LVGL is unavailable" :
                                  "LVGL is inactive; skipping direct LCD rendering");
        s_backend_warning_logged = true;
    }

    display_format_status_line(&view_model->top_right_icon, status_line, sizeof(status_line));
    display_format_time_lines(view_model, date_line, sizeof(date_line), time_line, sizeof(time_line));
    display_format_weather_line(&view_model->weather_panel, weather_line, sizeof(weather_line));

    time_unchanged = (!view_model->time_valid && !s_last_time_valid) ||
                     (view_model->time_valid &&
                      s_last_time_valid &&
                      display_time_equals(&view_model->current_time, &s_last_time));
    status_icon_changed = !s_has_cached_view ||
                          !display_status_icon_equals(&view_model->top_right_icon, &s_last_top_right_icon);
    time_changed = !s_has_cached_view ||
                   (view_model->time_valid != s_last_time_valid) ||
                   !time_unchanged;
    weather_changed = !s_has_cached_view ||
                      (view_model->weather_panel.visible != s_last_weather_visible) ||
                      (view_model->weather_panel.icon != s_last_weather_icon);

    if (status_icon_changed) {
        ESP_LOGI(TAG, "Screen status: %s", status_line);
    }
    if (time_changed) {
        ESP_LOGI(TAG, "Screen time: %s %s", date_line, time_line);
    }
    if (weather_changed) {
        ESP_LOGI(TAG, "Screen weather: %s", weather_line);
    }

    s_has_cached_view = true;
    s_last_top_right_icon = view_model->top_right_icon;
    s_last_weather_visible = view_model->weather_panel.visible;
    s_last_weather_icon = view_model->weather_panel.icon;
    s_last_time_valid = view_model->time_valid;
    if (view_model->time_valid) {
        s_last_time = view_model->current_time;
    } else {
        memset(&s_last_time, 0, sizeof(s_last_time));
    }

    return ESP_OK;
}
