/**
 * Camera页面UI
 * 显示K230视频流和3D扫描进度
 * 状态机: CONNECTING → IDLE → DETECTING → POSITIONING → POS_SUCCESS/POS_FAIL/LIMIT_FAIL
 */

#ifndef UI_CAMERA_H
#define UI_CAMERA_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============== 状态机枚举 ==============
typedef enum {
    STATE_CONNECTING,     // 连接K230中
    STATE_CONN_FAILED,    // 连接失败
    STATE_IDLE,           // 初始状态 (Ready)
    STATE_DETECTING,      // YOLO检测物体中 (Found...)
    STATE_POSITIONING,    // 定位调节中 (Position...)
    STATE_POS_SUCCESS,    // 定位成功 (Pos Succ)
    STATE_POS_FAILED,     // 检测超时失败 (Pos Failed)
    STATE_LIMIT_FAILED,   // 滑块极限失败 (Max H)
} camera_state_t;

/**
 * @brief 创建Camera页面
 */
void ui_camera_create(void);

/**
 * @brief 获取Camera页面屏幕对象
 * @return 屏幕对象指针
 */
lv_obj_t *ui_camera_get_screen(void);

/**
 * @brief 更新进度条
 * @param progress 进度值 (0-100)
 * @param message 状态消息
 * @param stage 当前阶段
 */
void ui_camera_update_progress(int progress, const char *message, const char *stage);

/**
 * @brief 显示视频流帧
 * @param data JPEG数据
 * @param len 数据长度
 */
void ui_camera_show_frame(const uint8_t *data, size_t len);

/**
 * @brief 获取当前状态机状态
 * @return 当前状态
 */
camera_state_t ui_camera_get_state(void);

/**
 * @brief 处理K230通过UART上报的状态字符串
 * @param status_str 状态字符串 (如 "FOUND:person", "POS:OK" 等)
 */
void ui_camera_handle_k230_status(const char *status_str);

#ifdef __cplusplus
}
#endif

#endif // UI_CAMERA_H
