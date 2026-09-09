#include "expression_emote.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "display_config.h"
#include "emote_gen_player.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "expression_emote";

#define EMOTE_ASSET_PARTITION_LABEL "emote_gen"
#define EMOTE_NORMAL_ANIM_NAME       "normal"
#define EMOTE_RENDER_FPS             30
#define EMOTE_RENDER_BUFFER_LINES    8
#define EMOTE_RENDER_TASK_STACK_SIZE 8192
#define EMOTE_RENDER_TASK_PRIORITY   5
#define EMOTE_WORKER_TASK_STACK_SIZE 4096
#define EMOTE_WORKER_TASK_PRIORITY   (tskIDLE_PRIORITY + 1)
#define EMOTE_BLINK_MIN_INTERVAL_MS  10000U
#define EMOTE_BLINK_MAX_INTERVAL_MS  20000U
#define EMOTE_NOTIFY_HIDDEN          0U
#define EMOTE_NOTIFY_VISIBLE         1U
#define EMOTE_FRAME_PIXELS           ((size_t)DISPLAY_WIDTH * (size_t)DISPLAY_HEIGHT)
#define EMOTE_FRAME_BYTES            (EMOTE_FRAME_PIXELS * sizeof(uint16_t))

static lv_obj_t *s_container;
static lv_obj_t *s_image;
static lv_image_dsc_t s_image_dsc;
static uint16_t *s_working_pixels;
static uint16_t *s_display_pixels;
static emote_gen_player_handle_t s_player;
static TaskHandle_t s_worker_task;
static bool s_visible;
static bool s_ready;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static bool expression_emote_get_visible(void)
{
    bool visible;

    taskENTER_CRITICAL(&s_state_lock);
    visible = s_visible;
    taskEXIT_CRITICAL(&s_state_lock);

    return visible;
}

static void expression_emote_set_runtime_state(bool ready, TaskHandle_t worker_task)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_ready = ready;
    s_worker_task = worker_task;
    taskEXIT_CRITICAL(&s_state_lock);
}

static void expression_emote_publish_frame(void)
{
    if ((s_image == NULL) || (s_working_pixels == NULL) || (s_display_pixels == NULL)) {
        return;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "Failed to take LVGL lock while publishing Emote frame");
        return;
    }

    memcpy(s_display_pixels, s_working_pixels, EMOTE_FRAME_BYTES);
    lv_image_cache_drop(&s_image_dsc);
    lv_obj_invalidate(s_image);

    lvgl_port_unlock();
}

static void expression_emote_flush_cb(int x_start,
                                      int y_start,
                                      int x_end,
                                      int y_end,
                                      const void *data,
                                      emote_gen_player_handle_t manager)
{
    const int width = x_end - x_start;
    const int height = y_end - y_start;
    bool valid = true;

    if ((manager == NULL) || (data == NULL) || (s_working_pixels == NULL)) {
        valid = false;
    } else if ((x_start < 0) || (y_start < 0) ||
               (x_end > DISPLAY_WIDTH) || (y_end > DISPLAY_HEIGHT) ||
               (width <= 0) || (height <= 0)) {
        valid = false;
    }

    if (!valid) {
        ESP_LOGW(TAG,
                 "Skipping invalid Emote flush: area=(%d,%d)-(%d,%d) data=%p",
                 x_start,
                 y_start,
                 x_end,
                 y_end,
                 data);
        if (manager != NULL) {
            (void)emote_gen_player_notify_flush_finished(manager);
        }
        return;
    }

    const uint16_t *source = (const uint16_t *)data;
    for (int row = 0; row < height; ++row) {
        uint16_t *destination = s_working_pixels +
                                ((size_t)(y_start + row) * DISPLAY_WIDTH) +
                                (size_t)x_start;
        memcpy(destination,
               source + ((size_t)row * (size_t)width),
               (size_t)width * sizeof(uint16_t));
    }

    gfx_disp_t *disp = emote_gen_player_get_disp(manager);
    if ((disp != NULL) && gfx_disp_is_flushing_last(disp)) {
        expression_emote_publish_frame();
    }

    (void)emote_gen_player_notify_flush_finished(manager);
}

static bool expression_emote_has_normal_animation(emote_gen_player_handle_t player)
{
    const size_t count = emote_gen_player_get_index_count(player);

    for (size_t index = 0; index < count; ++index) {
        const emote_gen_player_index_entry_t *entry =
            emote_gen_player_get_index_entry(player, index);
        if ((entry != NULL) && (strcmp(entry->name, EMOTE_NORMAL_ANIM_NAME) == 0)) {
            return true;
        }
    }

    return false;
}

static esp_err_t expression_emote_play_once(emote_gen_player_handle_t player)
{
    esp_err_t ret = emote_gen_player_anim_now_name(player, EMOTE_NORMAL_ANIM_NAME, false);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "Animation '%s' failed: %s",
                 EMOTE_NORMAL_ANIM_NAME,
                 esp_err_to_name(ret));
    }

    return ret;
}

static void expression_emote_worker(void *arg)
{
    (void)arg;

    const emote_gen_player_config_t player_config = {
        .flags = {
            .swap = false,
            .double_buffer = false,
            .buff_dma = false,
            .buff_spiram = true,
        },
        .gfx_emote = {
            .h_res = DISPLAY_WIDTH,
            .v_res = DISPLAY_HEIGHT,
            .fps = EMOTE_RENDER_FPS,
        },
        .buffers = {
            .buf_pixels = DISPLAY_WIDTH * EMOTE_RENDER_BUFFER_LINES,
        },
        .task = {
            .task_priority = EMOTE_RENDER_TASK_PRIORITY,
            .task_stack = EMOTE_RENDER_TASK_STACK_SIZE,
            .task_affinity = -1,
            .task_stack_in_ext = false,
        },
        .flush_cb = expression_emote_flush_cb,
        .update_cb = NULL,
    };
    const emote_gen_player_data_t asset_data = {
        .type = EMOTE_GEN_PLAYER_SOURCE_PARTITION,
        .source.partition_label = EMOTE_ASSET_PARTITION_LABEL,
        .flags = {
            .mmap_enable = 1,
        },
    };
    esp_err_t ret;

    s_player = emote_gen_player_init(&player_config);
    if (s_player == NULL) {
        ESP_LOGE(TAG, "Emote player initialization failed; expression page will stay black");
        expression_emote_set_runtime_state(false, NULL);
        vTaskDelete(NULL);
        return;
    }

    gfx_disp_t *disp = emote_gen_player_get_disp(s_player);
    ret = gfx_disp_set_bg_color(disp, GFX_COLOR_HEX(0x000000));
    if (ret == ESP_OK) {
        ret = gfx_disp_set_bg_enable(disp, true);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Emote background configuration failed: %s", esp_err_to_name(ret));
        goto init_failed;
    }

    ret = emote_gen_player_mount_assets(s_player, &asset_data);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to mount partition '%s': %s; expression page will stay black",
                 EMOTE_ASSET_PARTITION_LABEL,
                 esp_err_to_name(ret));
        goto init_failed;
    }
    if (!expression_emote_has_normal_animation(s_player)) {
        ESP_LOGE(TAG,
                 "Animation '%s' is missing from partition '%s'; expression page will stay black",
                 EMOTE_NORMAL_ANIM_NAME,
                 EMOTE_ASSET_PARTITION_LABEL);
        goto init_failed;
    }

    expression_emote_set_runtime_state(true, xTaskGetCurrentTaskHandle());
    ESP_LOGI(TAG,
             "Emote ready: partition=%s animation=%s",
             EMOTE_ASSET_PARTITION_LABEL,
             EMOTE_NORMAL_ANIM_NAME);

    while (true) {
        uint32_t notification = EMOTE_NOTIFY_HIDDEN;

        (void)xTaskNotifyWait(0, UINT32_MAX, &notification, portMAX_DELAY);
        if ((notification != EMOTE_NOTIFY_VISIBLE) || !expression_emote_get_visible()) {
            continue;
        }

        (void)expression_emote_play_once(s_player);

        while (expression_emote_get_visible()) {
            const uint32_t interval_range_ms =
                EMOTE_BLINK_MAX_INTERVAL_MS - EMOTE_BLINK_MIN_INTERVAL_MS + 1U;
            const uint32_t interval_ms =
                EMOTE_BLINK_MIN_INTERVAL_MS + (esp_random() % interval_range_ms);

            ESP_LOGI(TAG, "Next blink in %" PRIu32 " ms", interval_ms);
            BaseType_t notified = xTaskNotifyWait(0,
                                                  UINT32_MAX,
                                                  &notification,
                                                  pdMS_TO_TICKS(interval_ms));
            if (notified == pdTRUE) {
                break;
            }
            if (expression_emote_get_visible()) {
                (void)expression_emote_play_once(s_player);
            }
        }
    }

init_failed:
    emote_gen_player_deinit(s_player);
    s_player = NULL;
    expression_emote_set_runtime_state(false, NULL);
    vTaskDelete(NULL);
}

esp_err_t expression_emote_init(lv_obj_t *parent)
{
    if (parent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_container != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_container = lv_obj_create(parent);
    if (s_container == NULL) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_size(s_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(s_container, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);

    s_image = lv_image_create(s_container);
    if (s_image == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL image; expression page will stay black");
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(s_image, 0, 0);
    lv_obj_remove_flag(s_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_image, LV_OBJ_FLAG_SCROLLABLE);

    s_working_pixels = heap_caps_calloc(EMOTE_FRAME_PIXELS,
                                        sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_display_pixels = heap_caps_calloc(EMOTE_FRAME_PIXELS,
                                        sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if ((s_working_pixels == NULL) || (s_display_pixels == NULL)) {
        ESP_LOGE(TAG,
                 "Failed to allocate two %u-byte PSRAM framebuffers; expression page will stay black",
                 (unsigned int)EMOTE_FRAME_BYTES);
        heap_caps_free(s_working_pixels);
        heap_caps_free(s_display_pixels);
        s_working_pixels = NULL;
        s_display_pixels = NULL;
        return ESP_ERR_NO_MEM;
    }

    memset(&s_image_dsc, 0, sizeof(s_image_dsc));
    s_image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_image_dsc.header.w = DISPLAY_WIDTH;
    s_image_dsc.header.h = DISPLAY_HEIGHT;
    s_image_dsc.header.stride = DISPLAY_WIDTH * sizeof(uint16_t);
    s_image_dsc.data_size = (uint32_t)EMOTE_FRAME_BYTES;
    s_image_dsc.data = (const uint8_t *)s_display_pixels;
    lv_image_set_src(s_image, &s_image_dsc);

    BaseType_t task_ret = xTaskCreate(expression_emote_worker,
                                      "emote_expr",
                                      EMOTE_WORKER_TASK_STACK_SIZE,
                                      NULL,
                                      EMOTE_WORKER_TASK_PRIORITY,
                                      &s_worker_task);
    if (task_ret != pdPASS) {
        s_worker_task = NULL;
        ESP_LOGE(TAG, "Failed to create Emote worker; expression page will stay black");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void expression_emote_set_visible(bool visible)
{
    TaskHandle_t worker_task;
    bool ready;

    if (s_container == NULL) {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_visible = visible;
    worker_task = s_worker_task;
    ready = s_ready;
    taskEXIT_CRITICAL(&s_state_lock);

    if (visible) {
        lv_obj_remove_flag(s_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_container);
        lv_obj_invalidate(s_container);
    } else {
        lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    }

    if (worker_task != NULL) {
        (void)xTaskNotify(worker_task,
                          visible ? EMOTE_NOTIFY_VISIBLE : EMOTE_NOTIFY_HIDDEN,
                          eSetValueWithOverwrite);
    } else if (visible && !ready) {
        ESP_LOGW(TAG, "Emote is unavailable; expression page is black");
    }
}
