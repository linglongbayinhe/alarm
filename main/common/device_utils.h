#ifndef DEVICE_UTILS_H
#define DEVICE_UTILS_H

#include <stddef.h>

#include "esp_err.h"

esp_err_t device_utils_get_device_id(char *buffer, size_t buffer_size);
esp_err_t device_utils_get_mac_string(char *buffer, size_t buffer_size);
void device_utils_copy_safe_string(char *destination, size_t destination_size, const char *source);

#endif
