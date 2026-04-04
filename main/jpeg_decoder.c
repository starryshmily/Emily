/**
 * JPEG解码器实现 - 使用LVGL内置的Tiny JPEG Decoder (tjpgd)
 */

#include "jpeg_decoder.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

// 直接引用LVGL的tjpgd（需要启用LV_USE_SJPG）
// 启用快速解码模式提升性能
#define LV_USE_SJPG 1
#define JD_FASTDECODE 1  // 0=标准, 1=快速, 2=最快(精度略降)
#include "../../managed_components/lvgl__lvgl/src/extra/libs/sjpg/tjpgd.h"

static const char *TAG = "jpeg_dec";
static int g_decode_count = 0;

// 解码上下文
typedef struct {
    const uint8_t *jpeg_data;
    size_t jpeg_size;
    size_t jpeg_pos;
    uint16_t *output_buffer;
    int output_width;    // 解码后的宽（旋转前）
    int output_height;   // 解码后的高（旋转前）
    int rotate_90;       // 1=旋转90度
} jpeg_ctx_t;

// tjpgd输入函数 - 从内存读取JPEG数据
static unsigned int jpeg_in_func(JDEC *jd, uint8_t *buf, unsigned int len)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    unsigned int remaining = ctx->jpeg_size - ctx->jpeg_pos;

    if (len > remaining) {
        len = remaining;
    }

    if (buf) {
        memcpy(buf, ctx->jpeg_data + ctx->jpeg_pos, len);
    }
    ctx->jpeg_pos += len;

    return len;
}

// tjpgd输出函数 - 将解码的像素写入输出缓冲区（支持90度旋转）
static int jpeg_out_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx_t *ctx = (jpeg_ctx_t *)jd->device;
    uint8_t *src = (uint8_t *)bitmap;
    uint16_t *dst = ctx->output_buffer;
    int src_w = ctx->output_width;
    int src_h = ctx->output_height;

    // 遍历MCU块中的每个像素
    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = rect->left; x <= rect->right; x++) {
            // 检查边界
            if (x < src_w && y < src_h) {
                // tjpgd RGB888输出: R, G, B (每像素3字节)
                uint8_t r = src[0];
                uint8_t g = src[1];
                uint8_t b = src[2];

                // 转换为RGB565 (大端字节序，适用于大多数SPI显示屏)
                uint16_t pixel = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
                pixel = (pixel >> 8) | (pixel << 8);  // 字节交换

                if (ctx->rotate_90) {
                    // 90度顺时针旋转: (x,y) -> (h-1-y, x)
                    // 源160x120 -> 目标120x160
                    int dst_x = src_h - 1 - y;
                    int dst_y = x;
                    int dst_w = src_h;  // 旋转后宽度=源高度
                    dst[dst_y * dst_w + dst_x] = pixel;
                } else {
                    dst[y * src_w + x] = pixel;
                }
            }
            src += 3;
        }
    }

    return 1;
}

esp_err_t jpeg_decoder_init(void)
{
    ESP_LOGI(TAG, "JPEG decoder initialized (tjpgd)");
    return ESP_OK;
}

void jpeg_decoder_deinit(void)
{
}

esp_err_t jpeg_get_dimensions(const uint8_t *jpeg_data, size_t jpeg_size,
                               int *width, int *height)
{
    if (!jpeg_data || jpeg_size < 10 || !width || !height) {
        return ESP_ERR_INVALID_ARG;
    }

    // 查找SOF0标记 (0xFFC0)
    for (size_t i = 0; i < jpeg_size - 8; i++) {
        if (jpeg_data[i] == 0xFF && jpeg_data[i + 1] == 0xC0) {
            *height = (jpeg_data[i + 3] << 8) | jpeg_data[i + 4];
            *width = (jpeg_data[i + 5] << 8) | jpeg_data[i + 6];
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t jpeg_decode_to_rgb565(const uint8_t *jpeg_data, size_t jpeg_size,
                                  jpeg_decode_config_t *config)
{
    if (!jpeg_data || jpeg_size < 4 || !config || !config->output_buffer) {
        ESP_LOGE(TAG, "Invalid params: data=%p, size=%d, cfg=%p, buf=%p",
                 jpeg_data, jpeg_size, config, config ? config->output_buffer : NULL);
        return ESP_ERR_INVALID_ARG;
    }

    // 验证JPEG头
    if (jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8) {
        ESP_LOGE(TAG, "Invalid JPEG header: %02X %02X (expected FF D8)", jpeg_data[0], jpeg_data[1]);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGD(TAG, "JPEG size: %d bytes, header OK", jpeg_size);

    // 分配tjpgd工作缓冲区（约3KB）
    size_t work_size = 3100;
    void *work_buf = malloc(work_size);
    if (!work_buf) {
        ESP_LOGE(TAG, "Failed to allocate work buffer");
        return ESP_ERR_NO_MEM;
    }

    // 解码上下文
    jpeg_ctx_t ctx = {
        .jpeg_data = jpeg_data,
        .jpeg_size = jpeg_size,
        .jpeg_pos = 0,
        .output_buffer = (uint16_t *)config->output_buffer,
        .output_width = config->output_width,
        .output_height = config->output_height,
        .rotate_90 = config->rotate_90 ? 1 : 0,
    };

    // 创建JDEC对象
    JDEC jdec;
    memset(&jdec, 0, sizeof(JDEC));

    // 准备解码（解析JPEG头）
    JRESULT res = jd_prepare(&jdec, jpeg_in_func, work_buf, work_size, &ctx);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed: %d", res);
        free(work_buf);
        return ESP_FAIL;
    }

    // tjpgd缩放：0=原图, 1=1/2, 2=1/4, 3=1/8
    int scale = 1;
    ctx.output_width = jdec.width >> scale;
    ctx.output_height = jdec.height >> scale;

    // 旋转后宽高互换
    if (ctx.rotate_90) {
        config->output_width = ctx.output_height;
        config->output_height = ctx.output_width;
    } else {
        config->output_width = ctx.output_width;
        config->output_height = ctx.output_height;
    }

    // 只在前5帧打印详细信息
    if (g_decode_count < 5) {
        ESP_LOGI(TAG, "Decoding JPEG #%d: %dx%d -> %dx%d (scale=%d, rot=%d)",
                 g_decode_count + 1, jdec.width, jdec.height,
                 config->output_width, config->output_height, scale, ctx.rotate_90);
    }

    // 执行解码
    res = jd_decomp(&jdec, jpeg_out_func, scale);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp failed: %d", res);
        free(work_buf);
        return ESP_FAIL;
    }

    g_decode_count++;
    if (g_decode_count <= 5) {
        ESP_LOGI(TAG, "JPEG decode #%d success", g_decode_count);
    }
    free(work_buf);
    return ESP_OK;
}
