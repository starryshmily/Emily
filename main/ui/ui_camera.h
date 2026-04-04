/**
 * Camera页面UI
 * 显示K230视频流和3D扫描进度
 */

#ifndef UI_CAMERA_H
#define UI_CAMERA_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

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

#ifdef __cplusplus
}
#endif

#endif // UI_CAMERA_H
