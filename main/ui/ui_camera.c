/**
 * Camera页面UI实现
 * Display K230 video stream and 3D scan progress
 * 视频在顶部居中，按钮在两侧和底部，完全不重叠
 *
 * 状态机: CONNECTING → IDLE → DETECTING → POSITIONING → POS_SUCCESS/POS_FAIL/LIMIT_FAIL
 * CANCEL 可在 DETECTING/POSITIONING/POS_SUCCESS/LIMIT_FAIL 时复位到 IDLE
 *
 * 任何状态下 K230 断开 → CONN_FAIL
 */

#include "ui_camera.h"
#include "ui_home.h"
#include "camera_client.h"
#include "jpeg_decoder.h"
#include "c3_uart.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
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
#define VIDEO_POS_Y  7                         // 顶部7px间隙

// 按钮颜色定义
#define COLOR_GREEN     0x238636
#define COLOR_GREEN_UP  0x28A745
#define COLOR_RED       0xDC3545
#define COLOR_GRAY      0x555555
#define COLOR_GRAY_DARK 0x30363D

// 状态机 - 超时定时器
#define DETECT_TIMEOUT_MS  30000
#define POS_FAIL_DELAY_MS  2000

// 滑块参数
#define SLIDER_MAX_HEIGHT_MM  190.0f
#define SLIDER_UP_LIMIT_MM    180.0f  // 距顶<10mm时UP不可点
#define SLIDER_DOWN_LIMIT_MM  10.0f   // 距底<10mm时DOWN不可点
#define SLIDER_MOVE_MM       10.0f    // 每次移动10mm

// K230断开检测 - 视频流超时 (增加到20秒以适应慢速网络如手机热点)
// 注意: 手机热点可能非常慢，帧间隔可能超过10秒
#define STREAM_TIMEOUT_MS  20000

// 待执行的移动方向 (用于MOVE:OK后更新高度)
static volatile int pending_move_direction = 0;  // 0=无, 1=UP, -1=DOWN

// UI components
static lv_obj_t *screen_camera = NULL;
static lv_obj_t *label_status = NULL;
static lv_obj_t *label_height = NULL;  // 高度显示标签
static lv_obj_t *btn_back = NULL;
static lv_obj_t *btn_start = NULL;
static lv_obj_t *label_start = NULL;
static lv_obj_t *btn_cancel = NULL;
static lv_obj_t *btn_up = NULL;
static lv_obj_t *btn_down = NULL;
static lv_obj_t *btn_calibrate = NULL;  // 校准按钮
static lv_obj_t *label_calibrate = NULL; // 校准按钮文字

// Video buffer - 静态分配
static uint8_t video_buffer[VIDEO_W * VIDEO_H * 2];
static SemaphoreHandle_t video_mutex = NULL;

// LCD绘制互斥锁 - 防止视频帧绘制和UI更新同时进行导致SPI冲突
static SemaphoreHandle_t lcd_draw_mutex = NULL;

// 首帧标志 - 必须是全局static，在create时重置
static bool first_frame_received = false;

// K230 client state
static bool k230_connected = false;
static TaskHandle_t k230_connect_task_handle = NULL;

// 状态机
static camera_state_t current_state = STATE_CONNECTING;

// UART接收任务句柄
static TaskHandle_t uart_rx_task_handle = NULL;
static volatile bool uart_rx_running = false;

// 检测超时/延迟定时器
static esp_timer_handle_t detect_timer = NULL;
static esp_timer_handle_t force_stop_timer = NULL;

// 滑块状态追踪
static volatile float slider_height_mm = 0.0f;  // 当前高度mm
static volatile bool slider_moving = false;        // 电机是否在移动中
static float height_before_calib = 0.0f;  // 校准前的高度（用于校准模式取消）

// K230断开检测
static volatile int64_t last_frame_time_ms = 0; // 最后一帧时间戳

// 前向声明
static void video_frame_callback(const uint8_t *jpeg_data, size_t jpeg_len);
static void progress_callback(int progress, const char *message, const char *stage);
static void switch_state(camera_state_t new_state);
static void handle_k230_disconnect(const char *reason);

// ============== 按钮样式辅助函数 ==============

static void set_btn_style(lv_obj_t *btn, uint32_t color, bool clickable)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_clear_state(btn, LV_STATE_PRESSED | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    if (clickable) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_opa(btn, LV_OPA_90, 0);
}

static void set_btn_disabled(lv_obj_t *btn, uint32_t color)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_clear_state(btn, LV_STATE_PRESSED | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(btn, LV_OPA_50, 0);
}

// ============== 更新高度显示 ==============
static void update_height_label(void)
{
    if (!label_height) return;
    // 校准模式下隐藏高度
    if (current_state == STATE_CALIBRATING) {
        lv_obj_add_flag(label_height, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(label_height, LV_OBJ_FLAG_HIDDEN);
    float cm = slider_height_mm / 10.0f;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f\n(cm)", cm);
    lv_label_set_text(label_height, buf);
    // 连接成功绿色，未连接灰色
    lv_color_t c = k230_connected ? lv_color_hex(0x00FF00) : lv_color_hex(0xAAAAAA);
    lv_obj_set_style_text_color(label_height, c, 0);
}

// ============== 更新UP/DOWN按钮状态 (根据高度) ==============

static void update_up_down_buttons(void)
{
    // 获取LCD锁，防止与视频帧绘制的SPI操作冲突
    bool locked = (lcd_draw_mutex && xSemaphoreTakeRecursive(lcd_draw_mutex, pdMS_TO_TICKS(100)) == pdTRUE);

    update_height_label();

    // 电机移动中 → 都灰色不可点
    if (slider_moving) {
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        goto unlock;
    }

    // 校准模式: UP/DOWN无行程限制，始终绿色
    if (current_state == STATE_CALIBRATING) {
        set_btn_style(btn_up, COLOR_GREEN_UP, true);
        set_btn_style(btn_down, COLOR_GREEN_UP, true);
        goto unlock;
    }

    // UP按钮: 距顶部<10mm时灰色
    if (slider_height_mm >= SLIDER_UP_LIMIT_MM) {
        set_btn_disabled(btn_up, COLOR_GRAY);
    } else {
        set_btn_style(btn_up, COLOR_GREEN_UP, true);
    }

    // DOWN按钮: 在最底部(0mm)时灰色
    if (slider_height_mm <= 0.0f) {
        set_btn_disabled(btn_down, COLOR_GRAY);
    } else {
        set_btn_style(btn_down, COLOR_GREEN_UP, true);
    }

unlock:
    if (locked) xSemaphoreGiveRecursive(lcd_draw_mutex);
}

// ============== K230断开处理 ==============

static void handle_k230_disconnect(const char *reason)
{
    ESP_LOGW(TAG, "K230 disconnected: %s (state was %d)", reason, current_state);
    k230_connected = false;
    slider_height_mm = 0;
    slider_moving = false;
    pending_move_direction = 0;
    switch_state(STATE_CONN_FAILED);
}

// ============== 状态切换函数 ==============

static void switch_state(camera_state_t new_state)
{
    camera_state_t old_state = current_state;
    current_state = new_state;

    ESP_LOGI(TAG, "State: %d -> %d", old_state, new_state);

    // 获取LCD锁保护所有LVGL操作
    bool locked = (lcd_draw_mutex && xSemaphoreTakeRecursive(lcd_draw_mutex, pdMS_TO_TICKS(100)) == pdTRUE);

    switch (new_state) {
    case STATE_CONNECTING:
        label_status ? lv_label_set_text(label_status, "Conn...") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_cancel, COLOR_GRAY);
        // 校准按钮隐藏
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        break;

    case STATE_CONN_FAILED:
        label_status ? lv_label_set_text(label_status, "Failed") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_cancel, COLOR_GRAY);
        // 校准按钮隐藏
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        break;

    case STATE_IDLE:
        label_status ? lv_label_set_text(label_status, "Ready") : (void)0;
        lv_label_set_text(label_start, "Start");
        set_btn_style(btn_start, COLOR_GREEN, true);
        update_up_down_buttons();
        set_btn_disabled(btn_cancel, COLOR_GRAY);
        // 校准按钮显示"CAL"
        if (btn_calibrate) {
            lv_obj_clear_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
            if (label_calibrate) lv_label_set_text(label_calibrate, "CAL");
            set_btn_style(btn_calibrate, COLOR_GREEN, true);
        }
        // 重置超时计数器: 从YOLO状态返回时视频流可能还没恢复
        last_frame_time_ms = 0;
        break;

    case STATE_CALIBRATING:
        label_status ? lv_label_set_text(label_status, "CAL") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);  // SCAN灰色不可点
        update_up_down_buttons();  // UP/DOWN无限制
        set_btn_style(btn_cancel, COLOR_RED, true);  // Cancel仍可点
        // 校准按钮显示"Yes"
        if (btn_calibrate) {
            lv_obj_clear_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
            if (label_calibrate) lv_label_set_text(label_calibrate, "Yes");
            set_btn_style(btn_calibrate, COLOR_GREEN, true);
        }
        break;

    case STATE_DETECTING:
        label_status ? lv_label_set_text(label_status, "Found...") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        if (detect_timer) {
            esp_timer_start_once(detect_timer, DETECT_TIMEOUT_MS * 1000);  // ms → us
        }
        break;

    case STATE_POSITIONING:
        label_status ? lv_label_set_text(label_status, "Position...") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        if (detect_timer) {
            esp_timer_stop(detect_timer);
        }
        break;

    case STATE_POS_SUCCESS:
        label_status ? lv_label_set_text(label_status, "Pos Succ") : (void)0;
        lv_label_set_text(label_start, "Scan");
        set_btn_style(btn_start, COLOR_GREEN, true);
        update_up_down_buttons();
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        // 定位成功: 不自动返回IDLE, 等待用户手动CANCEL
        break;

    case STATE_POS_FAILED:
        label_status ? lv_label_set_text(label_status, "Pos Failed") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_cancel, COLOR_GRAY);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        // 延迟2秒后返回IDLE
        if (detect_timer) {
            esp_timer_start_once(detect_timer, POS_FAIL_DELAY_MS * 1000);  // ms → us
        }
        break;

    case STATE_LIMIT_FAILED:
        label_status ? lv_label_set_text(label_status, "Max H") : (void)0;
        lv_label_set_text(label_start, "Scan");
        set_btn_style(btn_start, COLOR_GREEN, true);
        update_up_down_buttons();
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        // 不自动返回IDLE, 等待用户手动CANCEL
        break;
    }

    if (locked) xSemaphoreGiveRecursive(lcd_draw_mutex);
}

// ============== 强制停止回调 ==============

static void force_stop_cb(void *arg)
{
    ESP_LOGW(TAG, "Force stop: K230 not responding, forcing to IDLE");
    switch_state(STATE_IDLE);
}

// ============== 检测超时回调 ==============

static void detect_timeout_cb(void *arg)
{
    if (current_state == STATE_DETECTING) {
        ESP_LOGW(TAG, "Detect timeout (30s), no object found, sending STOP");
        c3_uart_send(CMD_STOP);
        // 启动强制停止定时器 (5秒后K230没响应则强制切IDLE)
        if (force_stop_timer) {
            esp_timer_start_once(force_stop_timer, 5000000);
        }
    }
}

// ============== UART接收任务 ==============

static void uart_rx_task(void *arg)
{
    char rx_buf[C3_UART_BUF_SIZE];

    ESP_LOGI(TAG, "UART RX task started");

    if (!c3_uart_init()) {
        ESP_LOGE(TAG, "UART init failed in RX task");
        uart_rx_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    while (uart_rx_running) {
        int len = c3_uart_read(rx_buf, sizeof(rx_buf) - 1, 100);
        if (len > 0) {
            rx_buf[len] = '\0';
            // 去除\r\n
            char *clean = rx_buf;
            while (*clean == '\r' || *clean == '\n') clean++;
            char *end = clean + strlen(clean) - 1;
            while (end > clean && (*end == '\r' || *end == '\n')) { *end = '\0'; end--; }

            ESP_LOGI(TAG, "UART RX: [%s]", clean);

            if (strncmp(clean, "FOUND:", 6) == 0) {
                if (current_state == STATE_DETECTING) {
                    switch_state(STATE_POSITIONING);
                }
            } else if (strcmp(clean, "POS:OK") == 0) {
                if (current_state == STATE_POSITIONING) {
                    switch_state(STATE_POS_SUCCESS);
                }
            } else if (strcmp(clean, "POS:LIMIT") == 0) {
                if (current_state == STATE_POSITIONING) {
                    switch_state(STATE_LIMIT_FAILED);
                }
            } else if (strncmp(clean, "MOVE:OK", 7) == 0) {
                // 电机移动完成 - 使用K230返回的实际高度
                slider_moving = false;
                pending_move_direction = 0;
                // 解析 MOVE:OK:xx.x 格式
                const char *h_str = strchr(clean, ':');
                if (h_str) {
                    h_str++; // 跳过第一个冒号
                    h_str = strchr(h_str, ':'); // 找第二个冒号
                    if (h_str) {
                        h_str++; // 跳过冒号
                        slider_height_mm = atof(h_str);
                    }
                }
                ESP_LOGI(TAG, "MOVE:OK, height=%.1fmm", slider_height_mm);
                update_up_down_buttons();
            } else if (strncmp(clean, "HEIGHT:", 7) == 0) {
                // K230返回当前高度 (连接时查询)
                const char *h_val = clean + 7;
                slider_height_mm = atof(h_val);
                ESP_LOGI(TAG, "HEIGHT from K230: %.1fmm", slider_height_mm);
                update_up_down_buttons();
            } else if (strcmp(clean, "ZERO:OK") == 0) {
                // K230确认当前位置已归零
                slider_height_mm = 0;
                pending_move_direction = 0;
                slider_moving = false;
                ESP_LOGI(TAG, "ZERO:OK, height reset to 0");
                switch_state(STATE_IDLE);  // 退出校准，回到IDLE
            } else if (strcmp(clean, "STOP:OK") == 0) {
                // K230复位完成
                slider_height_mm = 0;
                pending_move_direction = 0;
                if (force_stop_timer) {
                    esp_timer_stop(force_stop_timer);
                }
                ESP_LOGI(TAG, "K230 STOP:OK received, switching to IDLE");
                switch_state(STATE_IDLE);
            } else if (strcmp(clean, "STATE:DETECTING") == 0) {
                // K230确认进入检测模式 (调试用)
                ESP_LOGI(TAG, "K230 confirmed DETECTING state");
            }
        }

        // 检测K230断开 (视频流超时)
        // 注意: YOLO模式下视频流暂停是正常的, 不检测超时
        if (k230_connected &&
            current_state != STATE_CONNECTING &&
            current_state != STATE_CONN_FAILED &&
            current_state != STATE_DETECTING &&
            current_state != STATE_POSITIONING &&
            current_state != STATE_POS_SUCCESS &&
            current_state != STATE_LIMIT_FAILED &&
            current_state != STATE_POS_FAILED) {
            int64_t now = esp_timer_get_time() / 1000;
            if (last_frame_time_ms > 0 && (now - last_frame_time_ms) > STREAM_TIMEOUT_MS) {
                handle_k230_disconnect("stream timeout");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "UART RX task ended");
    uart_rx_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============== K230连接任务 (不修改) ==============

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
        switch_state(STATE_CONN_FAILED);
        k230_connect_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    k230_client_set_frame_callback(video_frame_callback);
    vTaskDelay(pdMS_TO_TICKS(100));

    err = k230_client_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Connection failed: %s", esp_err_to_name(err));
        switch_state(STATE_CONN_FAILED);
        k230_connect_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "K230 connected");
    k230_connected = true;
    slider_moving = false;
    last_frame_time_ms = 0; // 重置: 首帧到达后才开始超时检测

    // 向K230查询当前高度
    c3_uart_send("GET_HEIGHT");

    // 不立即切IDLE, 等首帧到达后在video_frame_callback中切换

    // 启动UART接收任务
    uart_rx_running = true;
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 5, &uart_rx_task_handle);

    vTaskDelay(pdMS_TO_TICKS(100));

    // 启动视频流 (不修改原有视频流逻辑)
    err = k230_client_start_stream();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Stream started");
    } else {
        ESP_LOGE(TAG, "Stream failed: %s", esp_err_to_name(err));
    }

    k230_connect_task_handle = NULL;
    vTaskDelete(NULL);
}

// ============== Video frame callback (不修改视频流解码逻辑) ==============
static void video_frame_callback(const uint8_t *jpeg_data, size_t jpeg_len)
{
    static int frame_count = 0;
    frame_count++;

    // 记录最后帧时间 (用于K230断开检测)
    last_frame_time_ms = esp_timer_get_time() / 1000;

    // 首帧到达 → 切换到 IDLE (Ready)
    if (!first_frame_received) {
        first_frame_received = true;
        ESP_LOGI(TAG, "First frame received, switching to IDLE");
        switch_state(STATE_IDLE);
    }

    if (xSemaphoreTake(video_mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }

    jpeg_decode_config_t cfg = {
        .output_buffer = video_buffer,
        .output_size = VIDEO_W * VIDEO_H * 2,
        .output_width = 256,
        .output_height = 144,
        .rotate_90 = true,
    };

    esp_err_t err = jpeg_decode_to_rgb565(jpeg_data, jpeg_len, &cfg);
    if (err == ESP_OK) {
        // 获取LCD互斥锁，防止与UI更新的SPI操作冲突
        if (xSemaphoreTakeRecursive(lcd_draw_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            app_lcd_draw_bitmap(
                VIDEO_POS_X, VIDEO_POS_Y,
                VIDEO_POS_X + VIDEO_W, VIDEO_POS_Y + VIDEO_H,
                video_buffer
            );
            xSemaphoreGiveRecursive(lcd_draw_mutex);
        }
        if (frame_count <= 3) {
            ESP_LOGI(TAG, "Frame #%d OK", frame_count);
        }
    } else if (frame_count <= 5) {
        ESP_LOGW(TAG, "Frame #%d decode failed", frame_count);
    }

    xSemaphoreGive(video_mutex);
}

// ============== Progress callback (不修改) ==============
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
        // 立即标记断开 (防止断开处理函数触发)
        k230_connected = false;

        // 停止UART接收 (非阻塞, 只是设置标志)
        uart_rx_running = false;

        // 停止定时器
        if (detect_timer) {
            esp_timer_stop(detect_timer);
        }
        if (force_stop_timer) {
            esp_timer_stop(force_stop_timer);
        }

        // 清空frame callback, 防止stream任务访问已删除的资源
        k230_client_set_frame_callback(NULL);

        // 强制断开K230连接 (关闭socket让recv立即返回)
        k230_client_force_stop_stream();

        // 清理资源 (不等待任务完成, 任务会自行退出)
        if (video_mutex) {
            vSemaphoreDelete(video_mutex);
            video_mutex = NULL;
        }
        if (lcd_draw_mutex) {
            vSemaphoreDelete(lcd_draw_mutex);
            lcd_draw_mutex = NULL;
        }

        if (detect_timer) {
            esp_timer_delete(detect_timer);
            detect_timer = NULL;
        }

        if (force_stop_timer) {
            esp_timer_delete(force_stop_timer);
            force_stop_timer = NULL;
        }

        // 重置状态并立即切换页面
        current_state = STATE_CONNECTING;
        lv_scr_load(ui_home_get_screen());
    }
}

static void btn_start_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (current_state == STATE_IDLE) {
        ESP_LOGI(TAG, "Start: sending START_DETECT to K230");
        c3_uart_send(CMD_START_DETECT);
        switch_state(STATE_DETECTING);
    } else if (current_state == STATE_POS_SUCCESS || current_state == STATE_LIMIT_FAILED) {
        ESP_LOGI(TAG, "Scan: sending START_SCAN to K230");
        k230_client_start_scan();
    }
}

static void btn_cancel_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (current_state == STATE_CALIBRATING) {
        // 校准模式: 取消校准，恢复校准前高度，返回IDLE（不保存修改）
        ESP_LOGI(TAG, "Cancel calibration: restore height=%.1fmm", height_before_calib);
        slider_height_mm = height_before_calib;
        slider_moving = false;
        pending_move_direction = 0;
        switch_state(STATE_IDLE);
    } else if (current_state == STATE_DETECTING || current_state == STATE_POSITIONING) {
        // K230还在YOLO模式，发送STOP让它退出并清理
        ESP_LOGI(TAG, "Cancel: sending STOP to K230 (YOLO mode)");
        c3_uart_send(CMD_STOP);
        // 5秒后强制切IDLE (防止K230卡死无响应)
        if (force_stop_timer) {
            esp_timer_start_once(force_stop_timer, 5000000);
        }
    } else if (current_state == STATE_POS_SUCCESS || current_state == STATE_LIMIT_FAILED ||
               current_state == STATE_POS_FAILED) {
        // K230已经退出YOLO模式，直接切IDLE恢复视频流
        ESP_LOGI(TAG, "Cancel: returning to IDLE");
        switch_state(STATE_IDLE);
    }
}

static void btn_up_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (slider_moving) return;

    // 校准模式: 无行程限制
    if (current_state == STATE_CALIBRATING) {
        ESP_LOGI(TAG, "CAL UP (no limit)");
        slider_moving = true;
        update_up_down_buttons();
        c3_uart_send(CMD_UP);
        return;
    }

    if (current_state == STATE_IDLE || current_state == STATE_POS_SUCCESS ||
        current_state == STATE_LIMIT_FAILED) {
        if (slider_height_mm < SLIDER_UP_LIMIT_MM) {
            ESP_LOGI(TAG, "UP: sending UP to K230 (height=%.1fmm)", slider_height_mm);
            slider_moving = true;
            update_up_down_buttons();
            c3_uart_send(CMD_UP);
        }
    }
}

static void btn_down_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (slider_moving) return;

    // 校准模式: 无行程限制
    if (current_state == STATE_CALIBRATING) {
        ESP_LOGI(TAG, "CAL DOWN (no limit)");
        slider_moving = true;
        update_up_down_buttons();
        c3_uart_send(CMD_DOWN);
        return;
    }

    if (current_state == STATE_IDLE || current_state == STATE_POS_SUCCESS ||
        current_state == STATE_LIMIT_FAILED) {
        if (slider_height_mm > 0.0f) {
            ESP_LOGI(TAG, "DOWN: sending DOWN to K230 (height=%.1fmm)", slider_height_mm);
            slider_moving = true;
            pending_move_direction = -1;
            update_up_down_buttons();
            c3_uart_send(CMD_DOWN);
        }
    }
}

// ============== 校准按钮回调 ==============
static void btn_calibrate_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (current_state == STATE_CALIBRATING) {
        // 点击"Yes" → 发送SET_ZERO，将当前位置归零
        ESP_LOGI(TAG, "Calibrate confirm: SET_ZERO");
        c3_uart_send("SET_ZERO");
    } else if (current_state == STATE_IDLE) {
        // 点击"CAL" → 进入校准模式，保存当前高度
        ESP_LOGI(TAG, "Enter calibration mode");
        height_before_calib = slider_height_mm;  // 保存校准前高度
        switch_state(STATE_CALIBRATING);
    }
}

// ============== Create Camera page (不修改UI布局) ==============
void ui_camera_create(void)
{
    ESP_LOGI(TAG, "Free heap at page create: %lu bytes", (unsigned long)esp_get_free_heap_size());

    if (screen_camera) lv_obj_del(screen_camera);

    screen_camera = lv_obj_create(NULL);
    lv_obj_set_size(screen_camera, 240, 320);
    lv_obj_set_scrollbar_mode(screen_camera, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_camera, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_camera, LV_OPA_COVER, 0);

    video_mutex = xSemaphoreCreateMutex();
    lcd_draw_mutex = xSemaphoreCreateRecursiveMutex();  // 递归mutex允许同任务重复获取

    // 重置首帧标志 (重新进入页面时必须重新检测)
    first_frame_received = false;

    esp_timer_create_args_t timer_args = {
        .callback = detect_timeout_cb,
        .name = "detect_timer",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_create(&timer_args, &detect_timer);

    // 创建强制停止定时器
    esp_timer_create_args_t force_timer_args = {
        .callback = force_stop_cb,
        .name = "force_stop",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_create(&force_timer_args, &force_stop_timer);

    ESP_LOGI(TAG, "Video: %dx%d at (%d,%d)", VIDEO_W, VIDEO_H, VIDEO_POS_X, VIDEO_POS_Y);

    // ===== 返回按钮 - 左上角 =====
    btn_back = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_back, 44, 35);
    lv_obj_set_pos(btn_back, 0, 2);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(COLOR_GRAY_DARK), 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_back, 6, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, btn_back_callback, LV_EVENT_ALL, NULL);

    // ===== 校准按钮 - 右上角 =====
    btn_calibrate = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_calibrate, 44, 35);
    lv_obj_set_pos(btn_calibrate, 196, 2);  // 右上角，x=196（屏幕宽240-44=196）
    lv_obj_set_style_bg_color(btn_calibrate, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(btn_calibrate, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_calibrate, 6, 0);
    lv_obj_set_style_border_width(btn_calibrate, 0, 0);
    lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏，连接成功后显示

    label_calibrate = lv_label_create(btn_calibrate);
    lv_label_set_text(label_calibrate, "CAL");
    lv_obj_set_style_text_font(label_calibrate, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_calibrate, lv_color_white(), 0);
    lv_obj_center(label_calibrate);
    lv_obj_add_event_cb(btn_calibrate, btn_calibrate_callback, LV_EVENT_ALL, NULL);

    // ===== START按钮 - 底部居中 =====
    btn_start = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_start, 90, 40);
    lv_obj_align(btn_start, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_bg_opa(btn_start, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_start, 8, 0);
    lv_obj_set_style_border_width(btn_start, 0, 0);

    label_start = lv_label_create(btn_start);
    lv_label_set_text(label_start, "SCAN");
    lv_obj_set_style_text_font(label_start, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_start, lv_color_white(), 0);
    lv_obj_center(label_start);
    lv_obj_add_event_cb(btn_start, btn_start_callback, LV_EVENT_ALL, NULL);

    // ===== CANCEL按钮 - START按钮左边 =====
    btn_cancel = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_cancel, 60, 40);
    lv_obj_set_pos(btn_cancel, 10, 270);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_set_style_border_width(btn_cancel, 0, 0);

    lv_obj_t *cancel_label = lv_label_create(btn_cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(btn_cancel, btn_cancel_callback, LV_EVENT_ALL, NULL);

    // ===== 上移按钮 - 屏幕右侧 (视频右边缘192, 留8px间距) =====
    btn_up = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_up, 38, 60);
    lv_obj_set_pos(btn_up, 200, 80);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_bg_opa(btn_up, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_up, 8, 0);
    lv_obj_set_style_border_width(btn_up, 0, 0);

    lv_obj_t *up_label = lv_label_create(btn_up);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(up_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(up_label, lv_color_white(), 0);
    lv_obj_center(up_label);
    lv_obj_add_event_cb(btn_up, btn_up_callback, LV_EVENT_ALL, NULL);

    // ===== 下移按钮 - 屏幕右侧 (视频右边缘192, 留8px间距) =====
    btn_down = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_down, 38, 60);
    lv_obj_set_pos(btn_down, 200, 180);
    lv_obj_set_style_bg_color(btn_down, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_bg_opa(btn_down, LV_OPA_90, 0);
    lv_obj_set_style_radius(btn_down, 8, 0);
    lv_obj_set_style_border_width(btn_down, 0, 0);

    lv_obj_t *down_label = lv_label_create(btn_down);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(down_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(down_label, lv_color_white(), 0);
    lv_obj_center(down_label);
    lv_obj_add_event_cb(btn_down, btn_down_callback, LV_EVENT_ALL, NULL);

    // ===== 高度显示 - 上下按钮中间位置，靠屏幕左边 =====
    // UP按钮中心y=110, DOWN按钮中心y=210, 中间y=160
    label_height = lv_label_create(screen_camera);
    lv_label_set_text(label_height, "0.0\n(cm)");
    lv_obj_set_style_text_font(label_height, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label_height, lv_color_hex(0xAAAAAA), 0);  // 默认灰色
    lv_obj_set_pos(label_height, 1, 148);  // 靠最左边，两行文字垂直居中调整
    lv_obj_set_style_bg_opa(label_height, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(label_height, lv_color_black(), 0);
    lv_obj_set_style_pad_left(label_height, 3, 0);
    lv_obj_set_style_pad_right(label_height, 3, 0);
    lv_obj_set_style_pad_top(label_height, 2, 0);
    lv_obj_set_style_pad_bottom(label_height, 2, 0);

    // ===== 状态标签 - 右下角 =====
    label_status = lv_label_create(screen_camera);
    lv_label_set_text(label_status, "Conn...");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(label_status, 185, 283);

    switch_state(STATE_CONNECTING);
    update_height_label();
    lv_scr_load(screen_camera);

    // 异步连接K230 (不修改原有连接逻辑)
    ESP_LOGI(TAG, "Free heap before k230_conn task: %lu bytes", (unsigned long)esp_get_free_heap_size());
    if (!k230_connect_task_handle) {
        BaseType_t ret = xTaskCreate(k230_connect_task, "k230_conn", 4096, NULL, 5, &k230_connect_task_handle);
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "Failed to create k230_conn task (heap: %lu)", (unsigned long)esp_get_free_heap_size());
        }
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

camera_state_t ui_camera_get_state(void)
{
    return current_state;
}

void ui_camera_handle_k230_status(const char *status_str)
{
    if (strncmp(status_str, "FOUND:", 6) == 0) {
        if (current_state == STATE_DETECTING) {
            switch_state(STATE_POSITIONING);
        }
    } else if (strcmp(status_str, "POS:OK") == 0) {
        if (current_state == STATE_POSITIONING) {
            switch_state(STATE_POS_SUCCESS);
        }
    } else if (strcmp(status_str, "POS:LIMIT") == 0) {
        if (current_state == STATE_POSITIONING) {
            switch_state(STATE_LIMIT_FAILED);
        }
    } else if (strcmp(status_str, "STOP:OK") == 0) {
        switch_state(STATE_IDLE);
    }
}

void ui_camera_heartbeat(void)
{
    // 更新最后活动时间，防止超时误判
    // 在收到任何K230数据时调用
    last_frame_time_ms = esp_timer_get_time() / 1000;
}
