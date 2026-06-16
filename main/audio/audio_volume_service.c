#include "audio_volume_service.h"

#include "audio_config.h"

static uint8_t s_current_volume_percent = DEFAULT_VOLUME_PERCENT;

uint8_t audio_volume_service_get_current_percent(void)
{
    return s_current_volume_percent;
}

void audio_volume_service_set_current_percent(uint8_t volume_percent)
{
    if (volume_percent > 100U) {
        volume_percent = 100U;
    }

    s_current_volume_percent = volume_percent;
}
