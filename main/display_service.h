#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "display_weather_icon_types.h"

typedef enum {
    DISPLAY_STATUS_ICON_KIND_NONE = 0,
    DISPLAY_STATUS_ICON_KIND_WIFI = 1,
} display_status_icon_kind_t;

typedef enum {
    DISPLAY_STATUS_ICON_VARIANT_NORMAL = 0,
    DISPLAY_STATUS_ICON_VARIANT_ALERT = 1,
} display_status_icon_variant_t;

typedef struct {
    bool visible;
    display_status_icon_kind_t kind;
    display_status_icon_variant_t variant;
    uint8_t level;
} display_status_icon_t;

#define DISPLAY_WEATHER_TEMPERATURE_TEXT_SIZE 8
#define DISPLAY_WEATHER_CONDITION_TEXT_SIZE  16
#define DISPLAY_WEATHER_DETAILS_TEXT_SIZE    24
#define DISPLAY_WEATHER_FOOTER_TEXT_SIZE     24

typedef struct display_weather_panel {
    bool visible;
    bool stale;
    bool show_condition_text;
    weather_icon_kind_t icon;
    char temperature_text[DISPLAY_WEATHER_TEMPERATURE_TEXT_SIZE];
    char condition_text[DISPLAY_WEATHER_CONDITION_TEXT_SIZE];
    char details_text[DISPLAY_WEATHER_DETAILS_TEXT_SIZE];
    char footer_text[DISPLAY_WEATHER_FOOTER_TEXT_SIZE];
} display_weather_panel_t;

typedef struct {
    display_status_icon_t top_right_icon;
    display_weather_panel_t weather_panel;
    bool time_valid;
    struct tm current_time;
} display_view_model_t;

esp_err_t display_service_init(void);
esp_err_t display_service_render(const display_view_model_t *view_model);

/**
 * @brief 从SPIFFS文件系统显示RGB565格式的全屏图片
 * 
 * @param image_path SPIFFS路径，例如 "/spiffs/logo.rgb565" 或 "/spiffs/wallpaper.rgb565"
 * @return 
 *   - ESP_OK: 成功显示图片
 *   - ESP_ERR_INVALID_ARG: 路径参数为空
 *   - ESP_ERR_INVALID_STATE: 显示服务未初始化
 *   - ESP_ERR_NOT_FOUND: 文件不存在
 *   - ESP_ERR_NO_MEM: 内存不足
 *   - 其他: 文件读取或显示失败的具体错误
 */
esp_err_t display_service_show_image(const char *image_path);

/**
 * @brief 从内存中的像素数组显示全屏图片（用于预加载的图像数据）
 * 
 * @param image_data RGB565格式的像素数组指针，数组大小应为 DISPLAY_WIDTH * DISPLAY_HEIGHT * 2 字节
 * @param width 图片宽度（像素）
 * @param height 图片高度（像素）
 * @return 
 *   - ESP_OK: 成功显示
 *   - ESP_ERR_INVALID_ARG: 参数为空或不符合显示分辨率
 *   - ESP_ERR_INVALID_STATE: 显示服务未初始化
 */
esp_err_t display_service_show_image_data(const uint16_t *image_data, int width, int height);

#endif
