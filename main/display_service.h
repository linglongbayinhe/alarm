#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H

#include <stdint.h>

#include "display_view_model.h"
#include "esp_err.h"

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
