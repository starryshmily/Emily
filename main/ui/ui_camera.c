/**
 * Camera页面UI实现
 * Display K230 video stream and 3D scan progress
 * 视频在顶部居中，按钮在两侧和底部，完全不重叠
 */

#include "ui_camera.h"
#include "ui_home.h"
#include "camera_client.h"
#include "jpeg_decoder.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <string.h>
#include <stdlib.h>

// main.c中定义的直接写LCD函数
extern void app_lcd_draw_bitmap(int x1, int y1, int x2, int y2, const void *data);

static const char *TAG = "ui_camera";

// 视频区域参数 - 保持9:16比例(1080x1920摄像头)
// K230输出512x288，1/2解码→256x144，旋转→144x256
#define VIDEO_W  144
#define VIDEO_H  256
#define VIDEO_POS_X  ((240 - VIDEO_W) / 2)  // 48 - 居中
#define VIDEO_POS_Y  7                         // 顶部7px间隙，底部距SCAN也是7px

// UI components
static lv_obj_t *screen_camera = NULL;
static lv_obj_t *label_status = NULL;
static lv_obj_t *btn_back = NULL;
static lv_obj_t *btn_start = NULL;
static lv_obj_t *label_start = NULL;
static lv_obj_t *btn_cancel = NULL;
static lv_obj_t *btn_up = NULL;
static lv_obj_t *btn_down = NULL;

// Video buffer - 静态分配
static uint8_t video_buffer[VIDEO_W * VIDEO_H * 2];  // 73.7KB
static SemaphoreHandle_t video_mutex = NULL;

// K230 client state
static bool k230_connected = false;
static TaskHandle_t k230_connect_task_handle = NULL;

// 前向声明
static void video_frame_callback(const uint8_t *jpeg_data, size_t jpeg_len);
static void progress_callback(int progress, const char *message, const char *stage);

// ============== K230连接任务 ==============
static void k230_connect_task(void *arg)
{
    ESP_LOGI(TAG, "Starting K230 connection...");

    k230_client_config_t k230_config = {
        .host = "192.168.43.13",
        .http_port = 8080,
        .is_connected = false
    };

    esp_err_t err = k230_client_init(&k230_config, progress_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "K230 init failed: %s", esp_err_to_name(err));
        if (label_status) lv_label_set_text(label_status, "Failed");
        k230_connect_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    k230_client_set_frame_callback(video_frame_callback);
    vTaskDelay(pdMS_TO_TICKS(100));

    err = k230_client_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Connection failed: %s", esp_err_to_name(err));
        if (label_status) lv_label_set_text(label_status, "Failed");
        k230_connect_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "K230 connected");
    if (label_status) lv_label_set_text(label_status, "Ready");
    k230_connected = true;

    vTaskDelay(pdMS_TO_TICKS(100));

    err = k230_client_start_stream();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Stream started");
    } else {
        ESP_LOGE(TAG, "Stream failed: %s", esp_err_to_name(err));
        if (label_status) lv_label_set_text(label_status, "Error");
    }

    k230_connect_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============== Video frame callback ==============
static void video_frame_callback(const uint8_t *jpeg_data, size_t jpeg_len)
{
    static int frame_count = 0;
    frame_count++;

    if (xSemaphoreTake(video_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }

    // JPEG解码 - K230输出512x288横屏(16:9)，1/2解码到256x144，旋转90°到144x256竖屏(9:16)
    jpeg_decode_config_t cfg = {
        .output_buffer = video_buffer,
        .output_size = VIDEO_W * VIDEO_H * 2,
        .output_width = 256,   // 旋转前宽度（1/2解码后）
        .output_height = 144,  // 旋转前高度（1/2解码后）
        .rotate_90 = true,
    };

    esp_err_t err = jpeg_decode_to_rgb565(jpeg_data, jpeg_len, &cfg);
    if (err == ESP_OK) {
        app_lcd_draw_bitmap(
            VIDEO_POS_X, VIDEO_POS_Y,
            VIDEO_POS_X + VIDEO_W, VIDEO_POS_Y + VIDEO_H,
            video_buffer
        );

        if (frame_count <= 3) {
            ESP_LOGI(TAG, "Frame #%d OK", frame_count);
        }
    } else if (frame_count <= 5) {
        ESP_LOGW(TAG, "Frame #%d decode failed", frame_count);
    }

    xSemaphoreGive(video_mutex);
}

// ============== Progress callback ==============
static void progress_callback(int progress, const char *message, const char *stage)
{
    if (label_status && progress > 0) {
        lv_label_set_text_fmt(label_status, "%d%%", progress);
    }
    if (progress >= 100 && btn_start && label_start) {
        lv_label_set_text(label_start, "Done");
        lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x4CAF50), 0);
    }
    if (strcmp(stage, "idle") == 0) {
        k230_connected = true;
    }
}

// ============== Button callbacks ==============
static void btn_back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        k230_client_stop_stream();
        k230_client_disconnect();
        k230_connected = false;

        if (video_mutex) {
            vSemaphoreDelete(video_mutex);
            video_mutex = NULL;
        }

        lv_scr_load(ui_home_get_screen());
    }
}

static void btn_start_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (!k230_connected) {
            if (label_status) lv_label_set_text(label_status, "NoConn");
            return;
        }
        lv_label_set_text(label_start, "...");
        k230_client_start_scan();
    }
}

static void btn_cancel_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        // TODO: 取消扫描功能待实现
        if (label_status) lv_label_set_text(label_status, "Cancel");
    }
}

static void btn_up_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        // TODO: 上移功能待实现
    }
}

static void btn_down_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        // TODO: 下移功能待实现
    }
}

// ============== Create Camera page ==============
void ui_camera_create(void)
{
    if (screen_camera) lv_obj_del(screen_camera);

    screen_camera = lv_obj_create(NULL);
    lv_obj_set_size(screen_camera, 240, 320);
    lv_obj_set_scrollbar_mode(screen_camera, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_camera, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_camera, LV_OPA_COVER, 0);

    video_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Video: %dx%d at (%d,%d)", VIDEO_W, VIDEO_H, VIDEO_POS_X, VIDEO_POS_Y);

    // ===== 返回按钮 - 左上角，宽度对齐画面左边 =====
    // 视频从x=48开始，按钮宽44，x=0，与画面有4px间隙
    btn_back = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_back, 44, 35);
    lv_obj_set_pos(btn_back, 0, 2);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_back, 6, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, btn_back_callback, LV_EVENT_ALL, NULL);

    // ===== SCAN按钮 - 底部居中，加大尺寸 =====
    btn_start = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_start, 90, 40);
    lv_obj_align(btn_start, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x238636), 0);
    lv_obj_set_style_bg_opa(btn_start, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_start, 8, 0);
    lv_obj_set_style_border_width(btn_start, 0, 0);

    label_start = lv_label_create(btn_start);
    lv_label_set_text(label_start, "SCAN");
    lv_obj_set_style_text_font(label_start, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_start, lv_color_white(), 0);
    lv_obj_center(label_start);
    lv_obj_add_event_cb(btn_start, btn_start_callback, LV_EVENT_ALL, NULL);

    // ===== CANCEL按钮 - SCAN按钮左边，红色背景 =====
    // SCAN按钮居中于x=120，宽90，左边缘x=75
    // CANCEL按钮放左边，宽60，x=10
    btn_cancel = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_cancel, 60, 40);
    lv_obj_set_pos(btn_cancel, 10, 270);  // 底部y=320-40-10=270
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0xDC3545), 0);  // 红色
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_set_style_border_width(btn_cancel, 0, 0);

    lv_obj_t *cancel_label = lv_label_create(btn_cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(btn_cancel, btn_cancel_callback, LV_EVENT_ALL, NULL);

    // ===== 上移按钮 - 屏幕右侧黑边区域，绿色背景，白色上三角 =====
    // 视频区域x=48~192，右侧黑边x=192~240
    // 按钮居中于x=216，宽度40，Y在屏幕中线(160)以上
    btn_up = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_up, 40, 60);
    lv_obj_set_pos(btn_up, 196, 80);  // 底边不动(Y+60=140)
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(0x28A745), 0);  // 绿色
    lv_obj_set_style_bg_opa(btn_up, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_up, 8, 0);
    lv_obj_set_style_border_width(btn_up, 0, 0);

    lv_obj_t *up_label = lv_label_create(btn_up);
    lv_label_set_text(up_label, LV_SYMBOL_UP);  // 上三角符号 ▲
    lv_obj_set_style_text_font(up_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(up_label, lv_color_white(), 0);
    lv_obj_center(up_label);
    lv_obj_add_event_cb(btn_up, btn_up_callback, LV_EVENT_ALL, NULL);

    // ===== 下移按钮 - 屏幕右侧黑边区域，绿色背景，白色下三角 =====
    // 与上移按钮关于屏幕中线(160)对称
    // 上移按钮Y=60，中心Y=80，对称位置Y=320-80-40/2=220，但用同样的间距
    // 上移按钮顶边Y=60，屏幕中线上方距离=160-60-40=60
    // 下移按钮底边应距中线60，Y=160+60=220，但按钮高40，所以Y=220-40+40/2...
    // 简单做法：上移Y=60，下移Y=220，关于160对称
    btn_down = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_down, 40, 60);
    lv_obj_set_pos(btn_down, 196, 180);  // 顶边不动
    lv_obj_set_style_bg_color(btn_down, lv_color_hex(0x28A745), 0);  // 绿色
    lv_obj_set_style_bg_opa(btn_down, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_down, 8, 0);
    lv_obj_set_style_border_width(btn_down, 0, 0);

    lv_obj_t *down_label = lv_label_create(btn_down);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);  // 下三角符号 ▼
    lv_obj_set_style_text_font(down_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(down_label, lv_color_white(), 0);
    lv_obj_center(down_label);
    lv_obj_add_event_cb(btn_down, btn_down_callback, LV_EVENT_ALL, NULL);

    // ===== 状态标签 - SCAN右侧与屏幕右边居中 =====
    // SCAN右边缘x=165，屏幕右边x=240，居中x=202
    label_status = lv_label_create(screen_camera);
    lv_label_set_text(label_status, "Conn...");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(label_status, 185, 283);

    lv_scr_load(screen_camera);

    // 异步连接K230
    if (!k230_connect_task_handle) {
        xTaskCreate(k230_connect_task, "k230_conn", 4096, NULL, 5, &k230_connect_task_handle);
    }
}

lv_obj_t *ui_camera_get_screen(void)
{
    return screen_camera;
}

void ui_camera_update_progress(int progress, const char *message, const char *stage)
{
    progress_callback(progress, message, stage);
}

void ui_camera_show_frame(const uint8_t *jpeg_data, size_t len)
{
    video_frame_callback(jpeg_data, len);
}
