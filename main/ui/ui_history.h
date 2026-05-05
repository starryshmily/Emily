/**
 * History page UI.
 */

#ifndef UI_HISTORY_H
#define UI_HISTORY_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_history_create(void);
lv_obj_t *ui_history_get_screen(void);
void ui_history_destroy(void);

#ifdef __cplusplus
}
#endif

#endif // UI_HISTORY_H
