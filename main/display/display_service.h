#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H

#include "display_view_model.h"
#include "esp_err.h"

esp_err_t display_service_init(void);
esp_err_t display_service_render(const display_view_model_t *view_model);

#endif
