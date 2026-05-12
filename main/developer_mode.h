#ifndef DEVELOPER_MODE_H
#define DEVELOPER_MODE_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t developer_mode_init(void);
void developer_mode_set_upload_test(bool enabled);
bool developer_mode_is_upload_test(void);

void developer_mode_set_history_test(bool enabled);
bool developer_mode_is_history_test(void);

void developer_mode_set_uart_test(bool enabled);
bool developer_mode_is_uart_test(void);

void developer_mode_set_camera_test(bool enabled);
bool developer_mode_is_camera_test(void);

void developer_mode_set_motor_test(bool enabled);
bool developer_mode_is_motor_test(void);

void developer_mode_set_ws2812_test(bool enabled);
bool developer_mode_is_ws2812_test(void);

void developer_mode_set_log_test(bool enabled);
bool developer_mode_is_log_test(void);

bool developer_mode_any_enabled(void);

#endif // DEVELOPER_MODE_H
