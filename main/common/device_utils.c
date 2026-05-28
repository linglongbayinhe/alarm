#include "device_utils.h"

#include <stdint.h>
#include <stdio.h>

#include "esp_mac.h"

esp_err_t device_utils_get_device_id(char *buffer, size_t buffer_size)
{
    uint8_t base_mac[6] = {0};
    esp_err_t ret = ESP_OK;

    if ((buffer == NULL) || (buffer_size < 13U)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_read_mac(base_mac, ESP_MAC_BASE);
    if (ret != ESP_OK) {
        buffer[0] = '\0';
        return ret;
    }

    snprintf(buffer,
             buffer_size,
             "%02x%02x%02x%02x%02x%02x",
             base_mac[0],
             base_mac[1],
             base_mac[2],
             base_mac[3],
             base_mac[4],
             base_mac[5]);
    return ESP_OK;
}

esp_err_t device_utils_get_mac_string(char *buffer, size_t buffer_size)
{
    uint8_t base_mac[6] = {0};
    esp_err_t ret = ESP_OK;

    if ((buffer == NULL) || (buffer_size < 18U)) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_read_mac(base_mac, ESP_MAC_BASE);
    if (ret != ESP_OK) {
        buffer[0] = '\0';
        return ret;
    }

    snprintf(buffer,
             buffer_size,
             "%02X:%02X:%02X:%02X:%02X:%02X",
             base_mac[0],
             base_mac[1],
             base_mac[2],
             base_mac[3],
             base_mac[4],
             base_mac[5]);
    return ESP_OK;
}

void device_utils_copy_safe_string(char *destination, size_t destination_size, const char *source)
{
    if ((destination == NULL) || (destination_size == 0)) {
        return;
    }

    snprintf(destination, destination_size, "%s", source == NULL ? "" : source);
}
