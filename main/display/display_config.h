#ifndef DISPLAY_CONFIG_H
#define DISPLAY_CONFIG_H

#define DISPLAY_SPI_HOST SPI2_HOST

#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 240

#define DISPLAY_PIN_BACKLIGHT 26
#define DISPLAY_PIN_MOSI 16
#define DISPLAY_PIN_SCLK 4
#define DISPLAY_PIN_CS 18
#define DISPLAY_PIN_DC 5
#define DISPLAY_PIN_RST 17

#define DISPLAY_SPI_FREQUENCY_HZ 27000000
#define DISPLAY_BACKLIGHT_ON_LEVEL 1

/*
 * ST7789 color troubleshooting profiles.
 *
 * Recommended troubleshooting order:
 * 1. RGB + invert + little-endian
 * 2. BGR + invert + little-endian
 *
 * Keep invert enabled because this panel shows the required black background and white text
 * only when inversion is active. Keep RGB565 little-endian enabled because ESP32 stores
 * uint16_t RGB565 pixels in little-endian memory order.
 */
#define DISPLAY_COLOR_PROFILE_RGB_NO_INVERT 0
#define DISPLAY_COLOR_PROFILE_BGR_NO_INVERT 1
#define DISPLAY_COLOR_PROFILE_RGB_INVERT    2
#define DISPLAY_COLOR_PROFILE_BGR_INVERT    3

/* Default target: black background, white text, yellow sun, red Wi-Fi alert slash. */
#define DISPLAY_COLOR_PROFILE DISPLAY_COLOR_PROFILE_RGB_INVERT
#define DISPLAY_RGB_DATA_ENDIAN_LITTLE 1

#if DISPLAY_COLOR_PROFILE == DISPLAY_COLOR_PROFILE_RGB_NO_INVERT
#define DISPLAY_RGB_ORDER_BGR 0
#define DISPLAY_INVERT_COLOR 0
#elif DISPLAY_COLOR_PROFILE == DISPLAY_COLOR_PROFILE_BGR_NO_INVERT
#define DISPLAY_RGB_ORDER_BGR 1
#define DISPLAY_INVERT_COLOR 0
#elif DISPLAY_COLOR_PROFILE == DISPLAY_COLOR_PROFILE_RGB_INVERT
#define DISPLAY_RGB_ORDER_BGR 0
#define DISPLAY_INVERT_COLOR 1
#elif DISPLAY_COLOR_PROFILE == DISPLAY_COLOR_PROFILE_BGR_INVERT
#define DISPLAY_RGB_ORDER_BGR 1
#define DISPLAY_INVERT_COLOR 1
#else
#error "Unsupported DISPLAY_COLOR_PROFILE"
#endif

#define DISPLAY_SWAP_XY 1
#define DISPLAY_MIRROR_X 1
#define DISPLAY_MIRROR_Y 0
#define DISPLAY_GAP_X 0
#define DISPLAY_GAP_Y 0

#endif
