/**
 * K230视频流客户端
 * 负责连接K230的HTTP服务
 * 支持HTTP轮询获取进度
 */

#ifndef CAMERA_CLIENT_H
#define CAMERA_CLIENT_H

#include "esp_err.h"
#include "esp_http_client.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// K230客户端配置
typedef struct {
    char host[64];      // K230的IP地址
    int http_port;      // HTTP端口
    bool is_connected;  // 连接状态
} k230_client_config_t;

// 进度更新回调类型
// progress: 进度值 (0-100)
// message: 状态消息
// stage: 当前阶段
typedef void (*progress_callback_t)(int progress, const char *message, const char *stage);

// JPEG帧回调类型（传递原始JPEG数据，由接收方解码）
// data: JPEG数据
// len: JPEG数据长度
typedef void (*frame_callback_t)(const uint8_t *data, size_t len);

/**
 * @brief 初始化K230客户端
 * @param config K230配置
 * @param callback 进度回调函数
 * @return ESP_OK on success
 */
esp_err_t k230_client_init(const k230_client_config_t *config, progress_callback_t callback);

/**
 * @brief 连接到K230 (HTTP)
 * @return ESP_OK on success
 */
esp_err_t k230_client_connect(void);

/**
 * @brief 断开K230连接
 */
void k230_client_disconnect(void);

/**
 * @brief 开始3D扫描
 * @return ESP_OK on success
 */
esp_err_t k230_client_start_scan(void);

/**
 * @brief 获取当前连接状态
 * @return true if connected
 */
bool k230_client_is_connected(void);

/**
 * @brief 设置视频帧回调函数
 * @param callback 帧回调函数
 */
void k230_client_set_frame_callback(frame_callback_t callback);

/**
 * @brief 启动MJPEG视频流
 * @return ESP_OK on success
 */
esp_err_t k230_client_start_stream(void);

/**
 * @brief 停止MJPEG视频流
 */
void k230_client_stop_stream(void);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_CLIENT_H
