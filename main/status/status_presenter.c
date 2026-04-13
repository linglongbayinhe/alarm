#include "status_presenter.h"

#include <string.h>

#include "weather_presenter.h"
#include "wifi_signal_mapper.h"

esp_err_t status_presenter_build_display_model(const status_presenter_input_t *input,
                                               display_view_model_t *output)
{
    esp_err_t ret = ESP_OK;

    if ((input == NULL) || (output == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(output, 0, sizeof(*output));

    output->top_right_icon.visible = true;
    output->top_right_icon.kind = DISPLAY_STATUS_ICON_KIND_WIFI;
    output->top_right_icon.variant = input->wifi_connected ?
                                     DISPLAY_STATUS_ICON_VARIANT_NORMAL :
                                     DISPLAY_STATUS_ICON_VARIANT_ALERT;

    if (!input->wifi_connected || !input->wifi_rssi_valid) {
        output->top_right_icon.level = 3;
    } else {
        output->top_right_icon.level = wifi_signal_mapper_map_rssi_to_level(input->wifi_rssi);
    }

    output->time_valid = input->time_valid;
    if (input->time_valid) {
        output->current_time = input->current_time;
    }

    if (input->weather_snapshot_valid) {
        ret = weather_presenter_build_panel_model(&input->weather_snapshot,
                                                  &output->weather_panel);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}
