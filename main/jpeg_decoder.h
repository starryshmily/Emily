/**
 * 简化JPEG解码器 - 用于ESP32-C3
 * 只支持基本的JPEG解码功能（Baseline DCT）
 * 专为K230视频流优化
 */

#ifndef JPEG_DECODER_H
#define JPEG_DECODER_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// JPEG解码配置
typedef struct {
    uint8_t *output_buffer;       // 输出缓冲区（RGB565格式）
    size_t output_size;           // 输出缓冲区大小
    int output_width;             // 期望输出宽度（旋转前）
    int output_height;            // 期望输出高度（旋转前）
    bool rotate_90;               // 是否旋转90度（横屏转竖屏）
} jpeg_decode_config_t;

/**
 * @brief 初始化JPEG解码器
 * @return ESP_OK on success
 */
esp_err_t jpeg_decoder_init(void);

/**
 * @brief 解码JPEG数据到RGB565
 * @param jpeg_data JPEG输入数据
 * @param jpeg_size JPEG数据大小
 * @param config 解码配置
 * @return ESP_OK on success
 */
esp_err_t jpeg_decode_to_rgb565(const uint8_t *jpeg_data, size_t jpeg_size,
                                  jpeg_decode_config_t *config);

/**
 * @brief 获取JPEG图像尺寸（不解码）
 * @param jpeg_data JPEG输入数据
 * @param jpeg_size JPEG数据大小
 * @param width 输出宽度
 * @param height 输出高度
 * @return ESP_OK on success
 */
esp_err_t jpeg_get_dimensions(const uint8_t *jpeg_data, size_t jpeg_size,
                               int *width, int *height);

/**
 * @brief 反初始化JPEG解码器
 */
void jpeg_decoder_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // JPEG_DECODER_H
