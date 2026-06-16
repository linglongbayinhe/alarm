#include "button_service.h"

#include <inttypes.h>

#include "app_lifecycle.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "playback_task_service.h"

#define BUTTON_SERVICE_GPIO              GPIO_NUM_13
#define BUTTON_SERVICE_TASK_STACK_SIZE   3072
#define BUTTON_SERVICE_TASK_PRIORITY     5
#define BUTTON_SERVICE_POLL_MS           10
#define BUTTON_SERVICE_DEBOUNCE_MS       30
#define BUTTON_SERVICE_DOUBLE_CLICK_MS   400
#define BUTTON_SERVICE_LONG_PRESS_MS     5000

static const char *TAG = "BUTTON_SERVICE";

static bool s_initialized;
static TaskHandle_t s_task_handle;

static bool button_service_is_pressed(void)
{
    return gpio_get_level(BUTTON_SERVICE_GPIO) == 0;
}

static void button_service_on_single_click(void)
{
    ESP_LOGI(TAG, "Single click button");
}

static void button_service_on_double_click(void)
{
    ESP_LOGI(TAG, "Double click button: stop current alarm playback");
    esp_err_t ret = playback_task_service_stop_current();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to stop current alarm playback: %s", esp_err_to_name(ret));
    }
}

static void button_service_on_long_press(uint32_t press_ms)
{
    ESP_LOGI(TAG, "Long press button: request BLUFI reprovision press_ms=%" PRIu32, press_ms);
    esp_err_t ret = app_lifecycle_request_reprovision(press_ms);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to request BLUFI reprovision: %s", esp_err_to_name(ret));
    }
}

static void button_service_task(void *arg)
{
    bool stable_pressed = false;
    bool candidate_pressed = false;
    TickType_t candidate_since = 0;
    TickType_t press_start = 0;
    bool long_press_fired = false;
    uint8_t pending_clicks = 0;
    TickType_t last_click_tick = 0;

    (void)arg;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        bool raw_pressed = button_service_is_pressed();

        if (raw_pressed != candidate_pressed) {
            candidate_pressed = raw_pressed;
            candidate_since = now;
        }

        if ((now - candidate_since) >= pdMS_TO_TICKS(BUTTON_SERVICE_DEBOUNCE_MS)) {
            if (stable_pressed != candidate_pressed) {
                stable_pressed = candidate_pressed;

                if (stable_pressed) {
                    press_start = now;
                    long_press_fired = false;
                } else if (!long_press_fired) {
                    pending_clicks++;
                    last_click_tick = now;

                    if (pending_clicks >= 2) {
                        button_service_on_double_click();
                        pending_clicks = 0;
                    }
                }
            }
        }

        if (stable_pressed) {
            if (!long_press_fired &&
                ((now - press_start) >= pdMS_TO_TICKS(BUTTON_SERVICE_LONG_PRESS_MS))) {
                button_service_on_long_press((uint32_t)((now - press_start) * portTICK_PERIOD_MS));
                long_press_fired = true;
                pending_clicks = 0;
            }
        } else if ((pending_clicks == 1) &&
                   ((now - last_click_tick) >= pdMS_TO_TICKS(BUTTON_SERVICE_DOUBLE_CLICK_MS))) {
            button_service_on_single_click();
            pending_clicks = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_SERVICE_POLL_MS));
    }
}

esp_err_t button_service_init(void)
{
    esp_err_t ret;

    if (s_initialized) {
        return ESP_OK;
    }

    gpio_config_t config = {
        .pin_bit_mask = 1ULL << BUTTON_SERVICE_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ret = gpio_config(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GPIO init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (s_task_handle == NULL) {
        BaseType_t created = xTaskCreate(button_service_task,
                                       "button_svc",
                                       BUTTON_SERVICE_TASK_STACK_SIZE,
                                       NULL,
                                       BUTTON_SERVICE_TASK_PRIORITY,
                                       &s_task_handle);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create button task");
            return ESP_ERR_NO_MEM;
        }
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Button service ready on GPIO%d", (int)BUTTON_SERVICE_GPIO);
    return ESP_OK;
}
