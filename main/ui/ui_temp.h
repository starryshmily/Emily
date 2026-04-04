/**
 * UI Temperature - Header
 */

#ifndef UI_TEMP_H
#define UI_TEMP_H

#include "lvgl.h"

void ui_temp_create(void);
void ui_temp_create_with_temp(float initial_temp);
void ui_temp_set_value(float temp);

#endif // UI_TEMP_H
