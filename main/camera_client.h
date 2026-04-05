/**
 * K230视频流客户端
 * 负责连接K230的HTTP服务
 * 支持HTTP轮询获取进度
 * 支持状态查询 (用于状态机)
 */

#ifndef CAMERA_CLIENT_H
#define CAMERA_CLIENT_H

#include "esp_err.h"
#include "esp_http_client.h"
#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// K230客户端配置
typedef struct {
    char host[64];      // K230的IP地址
    int http_port;      // HTTP端口
    bool is_connected;  // 连接状态
} k230_client_config_t;

// K230状态枚举 (与UART上报的状态对应)
typedef enum {
    K230_STATE_IDLE,        // 空闲
    K230_STATE_DETECTING,  // YOLO检测中
    K230_STATE_POSITIONING, // 定位调节中
    K230_STATE_POS_OK,     // 定位成功
    K230_STATE_POS_LIMIT,  // 滑块到极限
    K230_STATE_STOP_OK,    // 复位完成
    K230_STATE_ERROR,      // 错误
} k230_state_t;

// 进度更新回调类型
// progress: 进度值 (0-100)
// message: 状态消息
// stage: 当前阶段
typedef void (*progress_callback_t)(int progress, const char *message, const char *stage);

// JPEG帧回调类型（传递原始JPEG数据，由接收方解码）
// data: JPEG数据
// len: JPEG数据长度
typedef void (*frame_callback_t)(const uint8_t *data, size_t len);

// 状态更新回调类型 (用于状态机)
// state: K230上报的状态
// param: 附加参数 (如物体名称)
typedef void (*state_callback_t)(k230_state_t state, const char *param);

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
 * @brief 设置状态更新回调函数 (用于状态机)
 * @param callback 状态回调函数
 */
void k230_client_set_state_callback(state_callback_t callback);

/**
 * @brief 启动MJPEG视频流
 * @return ESP_OK on success
 */
esp_err_t k230_client_start_stream(void);

/**
 * @brief 停止MJPEG视频流
 */
void k230_client_stop_stream(void);

/**
 * @brief 强制停止MJPEG视频流 (关闭socket, 等待最多2秒)
 */
void k230_client_force_stop_stream(void);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_CLIENT_H
