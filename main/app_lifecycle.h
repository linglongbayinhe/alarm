#ifndef APP_LIFECYCLE_H
#define APP_LIFECYCLE_H

#include <stdint.h>

#include "esp_err.h"

esp_err_t app_lifecycle_boot(void);
esp_err_t app_lifecycle_request_reprovision(uint32_t press_ms);
void app_lifecycle_notify_button_activity(void);

#endif
