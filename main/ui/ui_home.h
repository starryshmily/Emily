/**
 * UI Home - Header
 */

#ifndef UI_HOME_H
#define UI_HOME_H

#include "lvgl.h"

void ui_home_create(void);
lv_obj_t* ui_home_get_screen(void);
void ui_home_set_wifi_status(bool connected);

#endif // UI_HOME_H
