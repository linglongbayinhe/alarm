#include "wifi_signal_mapper.h"

uint8_t wifi_signal_mapper_map_rssi_to_level(int rssi)
{
    if (rssi <= -85) {
        return 1;
    }
    if (rssi <= -70) {
        return 2;
    }

    return 3;
}
