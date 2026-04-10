# TFT 屏幕图片显示使用指南

## 概述

本项目已集成两种图片显示方式（方案2）：

1. **`display_service_show_image()`** - 从SPIFFS文件系统读取并显示RGB565格式的图片文件
2. **`display_service_show_image_data()`** - 从内存（RAM/ROM）直接显示RGB565格式的图片数据

## 图片格式要求

- **色彩格式**：RGB565（16位，每像素2字节）
- **分辨率**：320×240（与显示屏一致）
- **文件大小**：320 × 240 × 2 = 153,600 字节（150 KB）

## 准备图片文件

### 方法1：使用 Python 转换图片

```python
from PIL import Image
import struct

# 打开图片并转换为RGB565
img = Image.open("your_image.jpg")
img = img.resize((320, 240))  # 调整为屏幕分辨率
img = img.convert("RGB")

# 保存为RGB565原始数据
with open("output.rgb565", "wb") as f:
    for pixel in img.getdata():
        r, g, b = pixel
        # RGB565: RRRRRGGGGGGBBBBB
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)
        f.write(struct.pack("<H", rgb565))  # 小端字节序
```

### 方法2：使用在线转换工具

访问：https://lvgl.io/tools/imageconverter

- 上传图片
- 选择输出格式：RGB565
- 选择字节序：大端（Big Endian）
- 下载文件

## 部署到 ESP32

### 1. 准备 SPIFFS 分区

确保 `sdkconfig` 中已启用SPIFFS：

```
CONFIG_SPIFFS_PARTITION_SIZE=921600    # 约900 KB
```

### 2. 将图片文件放入 SPIFFS 分区

在项目根目录创建 `spiffs_image` 文件夹：

```
spiffs_image/
  ├── logo.rgb565
  ├── wallpaper.rgb565
  └── splash.rgb565
```

### 3. 编译并烧录

```bash
idf.py build
idf.py flash
```

## 使用示例

### 示例1：显示SPIFFS中的图片

```c
#include "display_service.h"

void show_logo_example(void)
{
    esp_err_t ret = display_service_show_image("/spiffs/logo.rgb565");
    
    if (ret == ESP_OK) {
        printf("Logo displayed successfully\n");
    } else {
        printf("Failed to display logo: %s\n", esp_err_to_name(ret));
    }
}
```

### 示例2：显示内存中的图片数据

```c
#include "display_service.h"

// 预定义的图片数组（嵌入在固件中）
extern const uint16_t splash_screen_data[];

void show_splash_screen(void)
{
    esp_err_t ret = display_service_show_image_data(
        splash_screen_data,
        320,    // 宽度
        240     // 高度
    );
    
    if (ret == ESP_OK) {
        printf("Splash screen displayed\n");
    } else {
        printf("Failed to display splash screen: %s\n", esp_err_to_name(ret));
    }
}
```

### 示例3：在应用程序启动时显示启动画面

```c
#include "display_service.h"

void app_main(void)
{
    // 初始化显示服务
    display_service_init();
    
    // 显示启动画面（选项1：从SPIFFS读取）
    display_service_show_image("/spiffs/splash.rgb565");
    
    // 保持显示2秒
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // 然后显示正常的时间信息
    display_view_model_t view_model = {
        .wifi_connected = false,
        .time_valid = false,
        .current_time = {}
    };
    display_service_render(&view_model);
}
```

## 内存占用分析

### SPIFFS存储方案（方案2）

| 组件 | 占用 | 说明 |
|-----|------|------|
| 固件代码 | ~1.1 MB | 应用程序 |
| SPIFFS分区 | 900 KB | 可存储约6张图片 |
| **总计** | **~2 MB** | 在8MB Flash中占用25% |

### ROM嵌入方案

如果将图片数据编译进固件（通过 `EMBED_FILES` 等方式），每张图片占用 150 KB。

## 常见问题

### Q1: 显示的图片颜色错误？

**解决方案**：
- 检查字节序是否正确（通常应为小端 Little Endian）
- 确认RGB565转换工具的输出格式
- 检查 `display_config.h` 中的 `DISPLAY_RGB_ORDER_BGR` 和 `DISPLAY_INVERT_COLOR` 设置

### Q2: 图片显示不完整或有花纹？

**解决方案**：
- 确认文件大小是否正确（320 × 240 × 2 = 153,600 字节）
- 检查SPIFFS分区是否足够大
- 尝试 `idf.py clean build` 重新构建

### Q3: 显示但文件读取失败（ESP_ERR_NOT_FOUND）？

**解决方案**：
- 确认SPIFFS已初始化并挂载
- 检查文件路径是否正确（示例：`/spiffs/logo.rgb565`）
- 使用 `ls /spiffs` 命令验证文件存在

### Q4: 后续如何升级到方案3（JPEG解码）？

**步骤**：
1. 集成 `TJpgDec` 库（轻量JPEG解码器，~40KB）
2. 修改 `display_service_show_image()` 内部实现，添加JPEG格式判断
3. 调用方代码保持不变

## 更新日志

### 当前版本（v1.0）

- ✅ 实现方案2：SPIFFS RGB565图片显示
- ✅ 实现内存图片数据显示
- ✅ 完整的错误处理和日志
- ✅ 详细的中文注释
- 📋 待实现：方案3（JPEG解码）

## 相关文件

- [display_service.h](display_service.h) - 接口声明
- [display_service.c](display_service.c) - 实现代码
- [display_config.h](display_config.h) - 硬件配置
- [CMakeLists.txt](CMakeLists.txt) - 编译配置
