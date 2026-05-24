#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t settings_store_init(void);
void settings_store_set_ws2812_enabled(bool enabled);
bool settings_store_is_ws2812_enabled(void);
void settings_store_set_light_color(uint8_t hue);
uint8_t settings_store_get_light_color(void);
void settings_store_set_bottom_cam_enabled(bool enabled);
bool settings_store_is_bottom_cam_enabled(void);

#endif // SETTINGS_STORE_H
