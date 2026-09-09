#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "emote_gen_player.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_flash_spi_init.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wear_levelling.h"

static const char *TAG = "EMOTE_EXPERIMENT";

#define DISPLAY_SPI_HOST SPI2_HOST
#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240
#define DISPLAY_PIN_MOSI GPIO_NUM_23
#define DISPLAY_PIN_SCLK GPIO_NUM_4
#define DISPLAY_PIN_CS GPIO_NUM_18
#define DISPLAY_PIN_DC GPIO_NUM_5
#define DISPLAY_PIN_RST GPIO_NUM_26
#define DISPLAY_SPI_FREQUENCY_HZ 27000000
#define DISPLAY_CMD_BITS 8
#define DISPLAY_PARAM_BITS 8
#define DISPLAY_SWAP_XY 1
#define DISPLAY_MIRROR_X 1
#define DISPLAY_MIRROR_Y 0
#define DISPLAY_INVERT_COLOR 1
#define DISPLAY_GAP_X 0
#define DISPLAY_GAP_Y 0

#define EXT_FLASH_HOST SPI3_HOST
#define EXT_FLASH_MOSI GPIO_NUM_32
#define EXT_FLASH_MISO GPIO_NUM_25
#define EXT_FLASH_SCLK GPIO_NUM_33
#define EXT_FLASH_CS GPIO_NUM_27
#define EXT_FLASH_FREQ_MHZ 2
#define EXT_FLASH_PARTITION_LABEL "extcache"
#define EXT_FLASH_BASE_PATH "/ext"
#define EXT_FLASH_EMOTE_DIR "/ext/emote"

#define EMOTE_BLINK_MIN_INTERVAL_MS 10000U
#define EMOTE_BLINK_MAX_INTERVAL_MS 20000U
#define EMOTE_BLINK_TASK_STACK_SIZE 3072
#define EMOTE_BLINK_TASK_PRIORITY (tskIDLE_PRIORITY + 1)

#ifndef CONFIG_EMOTE_EXPERIMENT_COLOR_SWAP
#define CONFIG_EMOTE_EXPERIMENT_COLOR_SWAP 0
#endif

#ifndef CONFIG_EMOTE_EXPERIMENT_BUFF_SPIRAM
#define CONFIG_EMOTE_EXPERIMENT_BUFF_SPIRAM 0
#endif

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static emote_gen_player_handle_t s_player;
#if CONFIG_EMOTE_EXPERIMENT_MOUNT_EXTERNAL_FATFS
static esp_flash_t *s_external_flash;
static const esp_partition_t *s_external_partition;
static wl_handle_t s_external_wl_handle = WL_INVALID_HANDLE;
#endif
static uint32_t s_flush_count;
static int64_t s_last_fps_log_us;

#if CONFIG_EMOTE_EXPERIMENT_MOUNT_EXTERNAL_FATFS
static esp_err_t ensure_directory(const char *path)
{
    struct stat info = {0};

    if (stat(path, &info) == 0) {
        if ((info.st_mode & S_IFDIR) != 0) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Path exists but is not a directory: %s", path);
        return ESP_ERR_INVALID_STATE;
    }

    if (mkdir(path, 0775) != 0) {
        ESP_LOGW(TAG, "Failed to create directory: %s", path);
        return ESP_FAIL;
    }

    return ESP_OK;
}
#endif

#if CONFIG_EMOTE_EXPERIMENT_ASSET_SOURCE_PATH
static bool file_exists(const char *path)
{
    struct stat info = {0};
    return (path != NULL) && (stat(path, &info) == 0);
}
#endif

static esp_err_t init_lcd_panel(void)
{
    spi_bus_config_t bus_config = {
        .sclk_io_num = DISPLAY_PIN_SCLK,
        .mosi_io_num = DISPLAY_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = DISPLAY_WIDTH * CONFIG_EMOTE_EXPERIMENT_BUFFER_LINES * sizeof(uint16_t),
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
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    esp_err_t ret = ESP_OK;

    ret = spi_bus_initialize(DISPLAY_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_new_panel_io_spi(DISPLAY_SPI_HOST, &io_config, &s_panel_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD panel IO init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if ((ret = esp_lcd_panel_reset(s_panel)) != ESP_OK ||
        (ret = esp_lcd_panel_init(s_panel)) != ESP_OK ||
        (ret = esp_lcd_panel_swap_xy(s_panel, DISPLAY_SWAP_XY)) != ESP_OK ||
        (ret = esp_lcd_panel_mirror(s_panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y)) != ESP_OK ||
        (ret = esp_lcd_panel_set_gap(s_panel, DISPLAY_GAP_X, DISPLAY_GAP_Y)) != ESP_OK ||
        (ret = esp_lcd_panel_invert_color(s_panel, DISPLAY_INVERT_COLOR)) != ESP_OK ||
        (ret = esp_lcd_panel_disp_on_off(s_panel, true)) != ESP_OK) {
        ESP_LOGE(TAG, "ST7789 panel config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG,
             "ST7789 initialized: %dx%d swap_xy=%d mirror=(%d,%d) invert=%d rgb=RGB endian=LITTLE",
             DISPLAY_WIDTH,
             DISPLAY_HEIGHT,
             DISPLAY_SWAP_XY,
             DISPLAY_MIRROR_X,
             DISPLAY_MIRROR_Y,
             DISPLAY_INVERT_COLOR);
    return ESP_OK;
}

#if CONFIG_EMOTE_EXPERIMENT_MOUNT_EXTERNAL_FATFS
static esp_err_t mount_external_fatfs(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = EXT_FLASH_MOSI,
        .miso_io_num = EXT_FLASH_MISO,
        .sclk_io_num = EXT_FLASH_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
    };
    esp_flash_spi_device_config_t device_config = {
        .host_id = EXT_FLASH_HOST,
        .cs_id = 0,
        .cs_io_num = EXT_FLASH_CS,
        .io_mode = SPI_FLASH_SLOWRD,
        .freq_mhz = EXT_FLASH_FREQ_MHZ,
    };
    esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        .format_if_mount_failed = false,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
        .use_one_fat = false,
    };
    esp_err_t ret = ESP_OK;
    uint32_t flash_id = 0;
    uint32_t size_bytes = 0;

    ret = spi_bus_initialize(EXT_FLASH_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGW(TAG, "External flash SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = spi_bus_add_flash_device(&s_external_flash, &device_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "External flash add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_flash_init(s_external_flash);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "External flash init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_flash_read_id(s_external_flash, &flash_id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "External flash ID read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_flash_get_size(s_external_flash, &size_bytes);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "External flash size read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG,
             "External flash detected: id=0x%08" PRIx32 " size=%" PRIu32 " KB",
             flash_id,
             size_bytes / 1024);

    ret = esp_partition_register_external(s_external_flash,
                                          0,
                                          size_bytes,
                                          EXT_FLASH_PARTITION_LABEL,
                                          ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_DATA_FAT,
                                          &s_external_partition);
    if ((ret != ESP_OK) && (ret != ESP_ERR_INVALID_STATE)) {
        ESP_LOGW(TAG, "External FAT partition registration failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_vfs_fat_spiflash_mount_rw_wl(EXT_FLASH_BASE_PATH,
                                           EXT_FLASH_PARTITION_LABEL,
                                           &mount_config,
                                           &s_external_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "External FATFS mount failed without formatting: %s; copy assets or mount manually",
                 esp_err_to_name(ret));
        return ret;
    }

    (void)ensure_directory(EXT_FLASH_EMOTE_DIR);
    ESP_LOGI(TAG, "External FATFS mounted at %s", EXT_FLASH_BASE_PATH);
    return ESP_OK;
}
#endif

//SPI DMA 传输完成后再通知 Emote 复用绘图缓冲区
static bool emote_lcd_color_trans_done_cb(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *event_data,
                                          void *user_ctx)
{
    (void)panel_io;
    (void)event_data;

    emote_gen_player_handle_t player = (emote_gen_player_handle_t)user_ctx;
    if (player == NULL) {
        return false;
    }

    return emote_gen_player_notify_flush_finished(player) == ESP_OK;
}

//将 Emote 生成的像素数据提交给 ST7789
static void emote_flush_cb(int x_start,
                           int y_start,
                           int x_end,
                           int y_end,
                           const void *data,
                           emote_gen_player_handle_t manager)
{
    esp_err_t ret = ESP_OK;
    int64_t now_us = esp_timer_get_time();

    if ((s_panel == NULL) || (data == NULL)) {
        ESP_LOGW(TAG, "Skipping flush: panel=%p data=%p", s_panel, data);
        (void)emote_gen_player_notify_flush_finished(manager);
        return;
    }

    ret = esp_lcd_panel_draw_bitmap(s_panel, x_start, y_start, x_end, y_end, data);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LCD draw_bitmap failed: area=(%d,%d)-(%d,%d) err=%s",
                 x_start,
                 y_start,
                 x_end,
                 y_end,
                 esp_err_to_name(ret));
        (void)emote_gen_player_notify_flush_finished(manager);
    }

    s_flush_count++;
    if ((s_last_fps_log_us == 0) || ((now_us - s_last_fps_log_us) >= 1000000)) {
        uint32_t fps = s_flush_count;
        s_flush_count = 0;
        s_last_fps_log_us = now_us;
        ESP_LOGI(TAG, "Flush FPS estimate=%" PRIu32 " free_heap=%" PRIu32 " internal=%" PRIu32,
                 fps,
                 (uint32_t)esp_get_free_heap_size(),
                 (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

//从 emote_gen 分区或文件路径挂载资源包，并列出动画条目
static esp_err_t mount_emote_assets(emote_gen_player_handle_t player)
{
    emote_gen_player_data_t data = {
#if CONFIG_EMOTE_EXPERIMENT_ASSET_SOURCE_PATH
        .type = EMOTE_GEN_PLAYER_SOURCE_PATH,
        .source.path = CONFIG_EMOTE_EXPERIMENT_ASSET_PATH,
#else
        .type = EMOTE_GEN_PLAYER_SOURCE_PARTITION,
        .source.partition_label = CONFIG_EMOTE_EXPERIMENT_ASSET_PARTITION_LABEL,
#endif
        .flags = {
            .mmap_enable = CONFIG_EMOTE_EXPERIMENT_MMAP_ENABLE,
        },
    };
    esp_err_t ret = ESP_OK;

#if CONFIG_EMOTE_EXPERIMENT_ASSET_SOURCE_PATH
    if (!file_exists(CONFIG_EMOTE_EXPERIMENT_ASSET_PATH)) {
        ESP_LOGW(TAG, "Asset path is missing before mount: %s", CONFIG_EMOTE_EXPERIMENT_ASSET_PATH);
    }
#endif

    ret = emote_gen_player_mount_assets(player, &data);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Emote asset mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Mounted emote assets; entries=%u", (unsigned int)emote_gen_player_get_index_count(player));
    for (size_t i = 0; i < emote_gen_player_get_index_count(player); ++i) {
        const emote_gen_player_index_entry_t *entry = emote_gen_player_get_index_entry(player, i);
        if (entry != NULL) {
            ESP_LOGI(TAG, "Asset entry[%u]: name=%s file=%s", (unsigned int)i, entry->name, entry->file);
        }
    }

    return ESP_OK;
}

//每次随机等待 10–20 秒，然后单次播放 normal
static void emote_blink_task(void *arg)
{
    emote_gen_player_handle_t player = (emote_gen_player_handle_t)arg;
    const uint32_t interval_range_ms =
        EMOTE_BLINK_MAX_INTERVAL_MS - EMOTE_BLINK_MIN_INTERVAL_MS + 1U;

    while (true) {
        uint32_t interval_ms = EMOTE_BLINK_MIN_INTERVAL_MS + (esp_random() % interval_range_ms);
        ESP_LOGI(TAG, "Next '%s' animation in %" PRIu32 " ms",
                 CONFIG_EMOTE_EXPERIMENT_INITIAL_ANIM_NAME,
                 interval_ms);
        vTaskDelay(pdMS_TO_TICKS(interval_ms));

        esp_err_t ret = emote_gen_player_anim_now_name(
            player, CONFIG_EMOTE_EXPERIMENT_INITIAL_ANIM_NAME, false);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "Periodic animation '%s' failed: %s",
                     CONFIG_EMOTE_EXPERIMENT_INITIAL_ANIM_NAME,
                     esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Triggered animation '%s' once",
                     CONFIG_EMOTE_EXPERIMENT_INITIAL_ANIM_NAME);
        }
    }
}

void app_main(void)
{
    emote_gen_player_config_t player_config = {
        .flags = {
            .swap = CONFIG_EMOTE_EXPERIMENT_COLOR_SWAP,
            .double_buffer = false,
            .buff_dma = true,
            .buff_spiram = CONFIG_EMOTE_EXPERIMENT_BUFF_SPIRAM,
        },
        .gfx_emote = {
            .h_res = DISPLAY_WIDTH,
            .v_res = DISPLAY_HEIGHT,
            .fps = CONFIG_EMOTE_EXPERIMENT_FPS,
        },
        .buffers = {
            .buf_pixels = DISPLAY_WIDTH * CONFIG_EMOTE_EXPERIMENT_BUFFER_LINES,
        },
        .task = {
            .task_priority = CONFIG_EMOTE_EXPERIMENT_TASK_PRIORITY,
            .task_stack = CONFIG_EMOTE_EXPERIMENT_TASK_STACK,
            .task_affinity = -1,
            .task_stack_in_ext = false,
        },
        .flush_cb = emote_flush_cb,
        .update_cb = NULL,
    };
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Starting esp_emote_gen_player ST7789 experiment");
    ret = init_lcd_panel();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD init failed; stopping experiment");
        return;
    }

#if CONFIG_EMOTE_EXPERIMENT_MOUNT_EXTERNAL_FATFS
    ret = mount_external_fatfs();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without external FATFS mount");
    }
#endif

    s_player = emote_gen_player_init(&player_config);
    if (s_player == NULL) {
        ESP_LOGE(TAG, "emote_gen_player_init failed");
        return;
    }

    const esp_lcd_panel_io_callbacks_t panel_io_callbacks = {
        .on_color_trans_done = emote_lcd_color_trans_done_cb,
    };
    ret = esp_lcd_panel_io_register_event_callbacks(s_panel_io, &panel_io_callbacks, s_player);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LCD transfer callback registration failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = mount_emote_assets(s_player);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Assets unavailable; player initialized but no animation is running");
    } else {
        ret = emote_gen_player_anim_now_name(s_player, CONFIG_EMOTE_EXPERIMENT_INITIAL_ANIM_NAME, false);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "Initial animation '%s' failed: %s; trying fallback '%s'",
                     CONFIG_EMOTE_EXPERIMENT_INITIAL_ANIM_NAME,
                     esp_err_to_name(ret),
                     CONFIG_EMOTE_EXPERIMENT_FALLBACK_ANIM_NAME);
            ret = emote_gen_player_anim_now_name(s_player, CONFIG_EMOTE_EXPERIMENT_FALLBACK_ANIM_NAME, false);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG,
                         "Fallback animation '%s' failed: %s",
                         CONFIG_EMOTE_EXPERIMENT_FALLBACK_ANIM_NAME,
                         esp_err_to_name(ret));
            }
        } else {
            BaseType_t task_ret = xTaskCreate(emote_blink_task,
                                              "emote_blink",
                                              EMOTE_BLINK_TASK_STACK_SIZE,
                                              s_player,
                                              EMOTE_BLINK_TASK_PRIORITY,
                                              NULL);
            if (task_ret != pdPASS) {
                ESP_LOGE(TAG, "Failed to create periodic blink task");
            }
        }
    }

    while (true) {
        ESP_LOGI(TAG,
                 "Experiment alive: free_heap=%" PRIu32 " internal=%" PRIu32 " largest_internal=%" PRIu32,
                 (uint32_t)esp_get_free_heap_size(),
                 (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
