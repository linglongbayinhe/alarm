#ifndef AUDIO_VOLUME_SERVICE_H
#define AUDIO_VOLUME_SERVICE_H

#include <stdint.h>

uint8_t audio_volume_service_get_current_percent(void);
void audio_volume_service_set_current_percent(uint8_t volume_percent);

#endif
