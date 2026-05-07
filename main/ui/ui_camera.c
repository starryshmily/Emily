/**
 * Camera页面UI实现
 * Display K230 video stream and 3D scan progress
 * 视频在顶部居中，按钮在两侧和底部，完全不重叠
 *
 * 状态机: CONNECTING → IDLE → DETECTING → POSITIONING → POS_SUCCESS/POS_FAIL/LIMIT_FAIL → CAPTURING
 * CANCEL 可在 DETECTING/POSITIONING/POS_SUCCESS/LIMIT_FAIL/CAPTURING 时复位到 IDLE
 *
 * 任何状态下 K230 断开 → CONN_FAIL
 */

#include "ui_camera.h"
#include "ui_home.h"
#include "camera_client.h"
#include "jpeg_decoder.h"
#include "c3_uart.h"
#include "history_store.h"
#include "developer_mode.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#define COLOR_ORANGE    0xF5A623
#define COLOR_GRAY      0x555555
#define COLOR_GRAY_DARK 0x30363D

// 状态机 - 超时定时器
#define DETECT_TIMEOUT_MS  60000  // 60秒（扫描模式需要约40秒：0→20cm扫描25秒+返回5秒+定位10秒）
#define POS_FAIL_DELAY_MS  2000

// 滑块参数
#define SLIDER_MAX_HEIGHT_MM  200.0f
#define SLIDER_UP_LIMIT_MM    200.0f  // 20cm时UP按钮禁用（19cm仍可UP）
#define SLIDER_DOWN_LIMIT_MM  10.0f   // 距底<10mm时DOWN不可点
#define SLIDER_MOVE_MM       10.0f    // 每次移动10mm

// K230断开检测 - 视频流超时 (25秒以覆盖HOME操作的最大时间20秒)
// 注意: motor_busy时视频流暂停, HOME操作可能需要20秒, 超时需大于此值
#define STREAM_TIMEOUT_MS  25000  // 25秒视频流断连超时

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
static lv_obj_t *btn_zero = NULL;  // Zero归零按钮
static lv_obj_t *btn_calibrate = NULL;  // 校准按钮
static lv_obj_t *label_calibrate = NULL; // 校准按钮文字
static lv_obj_t *label_pic_zone = NULL;
static lv_obj_t *upload_panel = NULL;
static lv_obj_t *upload_arc = NULL;
static lv_obj_t *upload_percent_label = NULL;
static lv_obj_t *upload_stage_label = NULL;

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
static volatile uint32_t camera_session_id = 0;
static volatile bool camera_page_active = false;

// 状态机
static camera_state_t current_state = STATE_CONNECTING;

// UART接收任务句柄
static TaskHandle_t uart_rx_task_handle = NULL;
static volatile bool uart_rx_running = false;

// 检测超时/延迟定时器
static esp_timer_handle_t detect_timer = NULL;
static esp_timer_handle_t force_stop_timer = NULL;
static esp_timer_handle_t slider_move_timer = NULL;  // 滑块移动超时定时器

// 滑块状态追踪
static volatile float slider_height_mm = 0.0f;  // 当前高度mm
static volatile bool slider_moving = false;        // 电机是否在移动中
static volatile bool cmd_send_lock = false;        // 命令发送锁，防止UP/DOWN重复发送
static volatile int64_t last_move_send_time_ms = 0; // 上次发送UP/DOWN的时间
#define MIN_MOVE_INTERVAL_MS 1500  // 两次UP/DOWN最短间隔(ms)，防止触摸抖动双发
static volatile bool capture_cancelled = false;    // Cancel during CAPTURING, ignore late ALL_DONE
static volatile bool upload_cancelled = false;     // Cancel during UPLOADING, ignore late MODEL messages
static volatile bool pending_cancel_capture = false; // Cancel during CAPTURING motor move, wait for HEIGHT then STOP
static volatile bool preview_mode = false;
static volatile int preview_zone = 1;
static volatile int preview_offset_y = 0;
static int preview_total_h = 0;  // Total image height from JPEG header

// Pre-downloaded JPEG cache for instant zone switching
// Zone numbers: 0=Bottom, 1=Z1, 2=Z2, 3=Z3
static uint8_t *preview_jpeg_cache[4] = {NULL};  // [0]=bottom, [1]=z1, [2]=z2, [3]=z3
static size_t preview_jpeg_cache_len[4] = {0};
static volatile bool preview_cache_ready = false;
static bool preview_zone_available[4] = {false};  // which zones actually have photos
static int preview_zone_list[4] = {0};            // available zone numbers (max 4: bottom+z1+z2+z3)
static int preview_zone_count = 0;                // number of available zones
static int preview_zone_index = 0;                // current index into preview_zone_list

static TaskHandle_t preview_task_handle = NULL;
static lv_timer_t *preview_show_timer = NULL;
static int preview_displayed_zone = -1;
static int preview_displayed_offset_y = -1;
static float height_before_calib = 0.0f;  // 校准前的高度（用于校准模式取消）

// K230断开检测
static volatile int64_t last_frame_time_ms = 0; // 最后一帧时间戳

// 后台保持连接 - 防止误触返回后重新连接等待太久
static esp_timer_handle_t delayed_disconnect_timer = NULL;  // 延迟断开定时器
static volatile bool bg_keepalive_mode = false;  // 后台保活模式

// 线程安全：非LVGL线程通过flag请求主线程执行UI操作
static volatile camera_state_t pending_state = STATE_CONNECTING;
static volatile bool has_pending_state = false;
static volatile bool has_pending_ui_update = false;
static volatile bool has_pending_keepalive_icon = false;
static volatile bool pending_keepalive_icon_enabled = false;
static lv_timer_t *deferred_timer = NULL;
static char pending_label_text[32] = {0};
static volatile bool has_pending_label = false;

// 前向声明
static void video_frame_callback(const uint8_t *jpeg_data, size_t jpeg_len);
static void progress_callback(int progress, const char *message, const char *stage);
static void switch_state(camera_state_t new_state);
static void handle_k230_disconnect(const char *reason);
static void start_delayed_disconnect(void);  // 延迟断开（后台保活）
bool ui_camera_is_in_background(void);  // 后台保活模式检查
static void request_preview_load(void);
static void cleanup_camera_page_resources(void);
static void release_camera_screen_for_background(void);
static void upload_start_task(void *arg);
static void show_upload_progress(bool show);
static void set_upload_progress_animated(int percent, const char *stage);

static bool camera_session_is_valid(uint32_t task_session)
{
    return camera_page_active && camera_session_id == task_session;
}

static void finish_k230_connect_task(uint32_t task_session)
{
    if (camera_session_id == task_session) {
        k230_connect_task_handle = NULL;
    }
    vTaskDelete(NULL);
}

// ============== 按钮样式辅助函数 ==============

static void set_btn_style(lv_obj_t *btn, uint32_t color, bool clickable)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_clear_state(btn, LV_STATE_PRESSED | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    if (clickable) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_bg_opa(btn, LV_OPA_90, 0);
}

static void set_btn_disabled(lv_obj_t *btn, uint32_t color)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_clear_state(btn, LV_STATE_PRESSED | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
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
    // 清除所有外观状态，防止FOCUSED等状态导致文字变白
    lv_obj_clear_state(label_height, LV_STATE_PRESSED | LV_STATE_CHECKED | LV_STATE_FOCUSED);
    float cm = slider_height_mm / 10.0f;
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f\ncm", cm);
    lv_label_set_text(label_height, buf);
    // 为所有状态设置文字颜色，防止默认白色
    lv_color_t c = k230_connected ? lv_color_hex(0x00FF00) : lv_color_hex(0xAAAAAA);
    lv_obj_set_style_text_color(label_height, c, 0);
    lv_obj_set_style_text_color(label_height, c, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(label_height, c, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(label_height, c, LV_STATE_CHECKED);
}

// ============== 更新UP/DOWN按钮状态 (根据高度) ==============

static void update_up_down_buttons(void)
{
    // 获取LCD锁，防止与视频帧绘制的SPI操作冲突
    bool locked = (lcd_draw_mutex && xSemaphoreTakeRecursive(lcd_draw_mutex, pdMS_TO_TICKS(100)) == pdTRUE);

    update_height_label();

    // 电机移动中 → UP/DOWN/Zero都灰色不可点
    if (slider_moving) {
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
        goto unlock;
    }

    // 校准模式: UP/DOWN无行程限制，始终绿色，Zero不可用
    if (current_state == STATE_CALIBRATING) {
        set_btn_style(btn_up, COLOR_GREEN_UP, true);
        set_btn_style(btn_down, COLOR_GREEN_UP, true);
        set_btn_disabled(btn_zero, COLOR_GRAY);
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

    // Zero按钮: 高度为0时灰色，否则绿色
    if (slider_height_mm <= 0.0f) {
        set_btn_disabled(btn_zero, COLOR_GRAY);
    } else {
        set_btn_style(btn_zero, COLOR_GREEN, true);
    }

unlock:
    if (locked) xSemaphoreGiveRecursive(lcd_draw_mutex);
}

// ============== 线程安全：延迟到LVGL主线程执行 ==============

static void deferred_timer_cb(lv_timer_t *timer)
{
    if (has_pending_state) {
        has_pending_state = false;
        switch_state(pending_state);
    }
    if (has_pending_ui_update) {
        has_pending_ui_update = false;
        update_up_down_buttons();
    }
    if (has_pending_label) {
        has_pending_label = false;
        if (label_status) lv_label_set_text(label_status, pending_label_text);
    }
    if (has_pending_keepalive_icon) {
        has_pending_keepalive_icon = false;
        ui_home_set_camera_keepalive(pending_keepalive_icon_enabled);
    }
}

static void request_state_change(camera_state_t new_state)
{
    pending_state = new_state;
    has_pending_state = true;
}

static void request_ui_update(void)
{
    has_pending_ui_update = true;
}

static void request_label_update(const char *text)
{
    strncpy(pending_label_text, text, sizeof(pending_label_text) - 1);
    pending_label_text[sizeof(pending_label_text) - 1] = '\0';
    has_pending_label = true;
}

static void request_keepalive_icon_update(bool enabled)
{
    pending_keepalive_icon_enabled = enabled;
    has_pending_keepalive_icon = true;
}


static void update_pic_zone_label(void)
{
    if (!label_pic_zone) return;
    if (preview_zone == 0) {
        lv_label_set_text(label_pic_zone, "Pic Btm");
    } else {
        lv_label_set_text_fmt(label_pic_zone, "Pic Z%d", preview_zone);
    }
}

static void preview_free_cache(void)
{
    for (int i = 0; i <= 3; i++) {
        if (preview_jpeg_cache[i]) {
            free(preview_jpeg_cache[i]);
            preview_jpeg_cache[i] = NULL;
        }
        preview_jpeg_cache_len[i] = 0;
        preview_zone_available[i] = false;
    }
    preview_cache_ready = false;
    preview_zone_count = 0;
    preview_zone_index = 0;
    preview_displayed_zone = -1;
    preview_displayed_offset_y = -1;
    if (preview_show_timer) {
        lv_timer_del(preview_show_timer);
        preview_show_timer = NULL;
    }
}

static void preview_invalidate_display(void)
{
    preview_displayed_zone = -1;
    preview_displayed_offset_y = -1;
    if (preview_show_timer) {
        lv_timer_del(preview_show_timer);
        preview_show_timer = NULL;
    }
}

static void release_camera_screen_for_background(void)
{
    ESP_LOGI(TAG, "Releasing camera UI for background keepalive");

    camera_page_active = false;

    if (preview_show_timer) {
        lv_timer_del(preview_show_timer);
        preview_show_timer = NULL;
    }
    if (deferred_timer) {
        lv_timer_del(deferred_timer);
        deferred_timer = NULL;
    }

    if (detect_timer) {
        esp_timer_stop(detect_timer);
        esp_timer_delete(detect_timer);
        detect_timer = NULL;
    }
    if (force_stop_timer) {
        esp_timer_stop(force_stop_timer);
        esp_timer_delete(force_stop_timer);
        force_stop_timer = NULL;
    }
    if (slider_move_timer) {
        esp_timer_stop(slider_move_timer);
        esp_timer_delete(slider_move_timer);
        slider_move_timer = NULL;
    }

    if (video_mutex) {
        vSemaphoreDelete(video_mutex);
        video_mutex = NULL;
    }
    if (lcd_draw_mutex) {
        vSemaphoreDelete(lcd_draw_mutex);
        lcd_draw_mutex = NULL;
    }

    if (screen_camera) {
        lv_obj_t *old_screen = screen_camera;
        screen_camera = NULL;
        lv_obj_del_async(old_screen);
    }

    label_status = NULL;
    label_height = NULL;
    btn_back = NULL;
    btn_start = NULL;
    label_start = NULL;
    btn_cancel = NULL;
    btn_up = NULL;
    btn_down = NULL;
    btn_zero = NULL;
    btn_calibrate = NULL;
    label_calibrate = NULL;
    label_pic_zone = NULL;
    upload_panel = NULL;
    upload_arc = NULL;
    upload_percent_label = NULL;
    upload_stage_label = NULL;

    preview_displayed_zone = -1;
    preview_displayed_offset_y = -1;
}

static void preview_cache_download_task(void *arg)
{
    // Delay 500ms to let K230 server settle after upload_test request
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Pre-downloading all preview images...");
    preview_zone_count = 0;
    // Download order: Z1, Z2, Z3, Bottom(0)
    static const int zone_order[] = {1, 2, 3, 0};
    for (int i = 0; i < 4; i++) {
        int zone = zone_order[i];
        // Small delay between downloads to let previous TCP connection fully close
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        uint8_t *jpeg_data = NULL;
        size_t jpeg_len = 0;
        // Retry up to 2 times per zone
        esp_err_t err = ESP_FAIL;
        for (int retry = 0; retry < 2 && err != ESP_OK; retry++) {
            if (retry > 0) {
                ESP_LOGI(TAG, "Retrying Z%d (%d/2)", zone, retry);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            ESP_LOGI(TAG, "Preview download Z%d start, heap=%lu",
                     zone, (unsigned long)esp_get_free_heap_size());
            err = k230_client_get_preview_image(zone, 0, &jpeg_data, &jpeg_len);
            ESP_LOGI(TAG, "Preview download Z%d result=%s len=%u heap=%lu",
                     zone, esp_err_to_name(err), (unsigned)jpeg_len,
                     (unsigned long)esp_get_free_heap_size());
        }
        if (err == ESP_OK && jpeg_data && jpeg_len > 0) {
            preview_jpeg_cache[zone] = jpeg_data;
            preview_jpeg_cache_len[zone] = jpeg_len;
            preview_zone_available[zone] = true;
            preview_zone_list[preview_zone_count++] = zone;
            ESP_LOGI(TAG, "Cached Z%d: %u bytes", zone, (unsigned)jpeg_len);
        } else {
            preview_zone_available[zone] = false;
            ESP_LOGI(TAG, "Z%d not available: %s", zone, esp_err_to_name(err));
            if (jpeg_data) free(jpeg_data);
        }
    }
    preview_cache_ready = true;

    if (preview_zone_count == 0) {
        ESP_LOGW(TAG, "No preview images available");
        preview_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Start at first available zone
    preview_zone_index = 0;
    preview_zone = preview_zone_list[0];
    ESP_LOGI(TAG, "Available zones: %d, starting at Z%d", preview_zone_count, preview_zone);

    // Update zone label with actual first available zone
    if (label_pic_zone) {
        if (preview_zone == 0) {
            lv_label_set_text(label_pic_zone, "Pic Btm");
        } else {
            char buf[8];
            snprintf(buf, sizeof(buf), "Pic Z%d", preview_zone);
            lv_label_set_text(label_pic_zone, buf);
        }
        lv_obj_clear_flag(label_pic_zone, LV_OBJ_FLAG_HIDDEN);
    }
    int zone = preview_zone;
    if (preview_jpeg_cache[zone] && preview_jpeg_cache_len[zone] > 0) {
        int img_w = 0, img_h = 0;
        jpeg_get_dimensions(preview_jpeg_cache[zone], preview_jpeg_cache_len[zone], &img_w, &img_h);
        ESP_LOGI(TAG, "Preview JPEG: zone=%d %dx%d", zone, img_w, img_h);

        if (img_w > 0 && img_h > 0 && img_w <= 300 && img_h <= 600 &&
            xSemaphoreTake(video_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            preview_total_h = img_h;
            memset(video_buffer, 0, VIDEO_W * VIDEO_H * 2);
            jpeg_decode_config_t cfg = {
                .output_buffer = video_buffer,
                .output_size = VIDEO_W * VIDEO_H * 2,
                .output_width = img_w,
                .output_height = img_h,
                .rotate_90 = false,
                .scale = 0,
            };
            if (jpeg_decode_to_rgb565(preview_jpeg_cache[zone], preview_jpeg_cache_len[zone], &cfg) == ESP_OK) {
                if (xSemaphoreTakeRecursive(lcd_draw_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    app_lcd_draw_bitmap(VIDEO_POS_X, VIDEO_POS_Y,
                                        VIDEO_POS_X + VIDEO_W, VIDEO_POS_Y + VIDEO_H,
                                        video_buffer);
                    xSemaphoreGiveRecursive(lcd_draw_mutex);
                }
            }
            xSemaphoreGive(video_mutex);
        }
    }

    preview_task_handle = NULL;
    vTaskDelete(NULL);
}

static void preview_show_cached_zone(int zone)
{
    if (!preview_jpeg_cache[zone] || preview_jpeg_cache_len[zone] == 0) return;

    int img_w = 0, img_h = 0;
    jpeg_get_dimensions(preview_jpeg_cache[zone], preview_jpeg_cache_len[zone], &img_w, &img_h);
    if (img_w <= 0 || img_h <= 0 || img_w > 300 || img_h > 600) return;

    if (xSemaphoreTake(video_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    preview_total_h = img_h;
    memset(video_buffer, 0, VIDEO_W * VIDEO_H * 2);
    jpeg_decode_config_t cfg = {
        .output_buffer = video_buffer,
        .output_size = VIDEO_W * VIDEO_H * 2,
        .output_width = img_w,
        .output_height = img_h,
        .rotate_90 = false,
        .scale = 0,
    };
    if (jpeg_decode_to_rgb565(preview_jpeg_cache[zone], preview_jpeg_cache_len[zone], &cfg) == ESP_OK) {
        if (xSemaphoreTakeRecursive(lcd_draw_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            app_lcd_draw_bitmap(VIDEO_POS_X, VIDEO_POS_Y,
                                VIDEO_POS_X + VIDEO_W, VIDEO_POS_Y + VIDEO_H,
                                video_buffer);
            xSemaphoreGiveRecursive(lcd_draw_mutex);
            preview_displayed_zone = zone;
            preview_displayed_offset_y = preview_offset_y;
        }
    }
    xSemaphoreGive(video_mutex);
}

static void preview_show_cached_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    if (preview_show_timer == timer) {
        preview_show_timer = NULL;
    }
    if (preview_mode && preview_cache_ready) {
        update_pic_zone_label();
        if (label_pic_zone) lv_obj_clear_flag(label_pic_zone, LV_OBJ_FLAG_HIDDEN);
        preview_show_cached_zone(preview_zone);
    }
}

static void request_preview_load(void)
{
    if (!preview_mode) return;

    ESP_LOGI(TAG, "request_preview_load: cache_ready=%d, task_handle=%p",
             preview_cache_ready, (void *)preview_task_handle);

    if (preview_cache_ready) {
        if (preview_displayed_zone == preview_zone &&
            preview_displayed_offset_y == preview_offset_y) {
            ESP_LOGI(TAG, "Preview already displayed: zone=%d offset=%d", preview_zone, preview_offset_y);
            return;
        }
        if (preview_show_timer) {
            ESP_LOGI(TAG, "Preview show already pending: zone=%d offset=%d", preview_zone, preview_offset_y);
            return;
        }
        // Defer to next LVGL cycle so screen refresh completes first
        preview_show_timer = lv_timer_create(preview_show_cached_timer_cb, 50, NULL);
        lv_timer_set_repeat_count(preview_show_timer, 1);
        return;
    }

    // Cache not ready yet, start download task
    if (preview_task_handle) return;
    xTaskCreate(preview_cache_download_task, "preview_img", 3072, NULL, 4, &preview_task_handle);
}

static void preview_gesture_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_GESTURE || !preview_mode) {
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT) {
        if (preview_zone_count <= 1) return;  // only one zone, no swiping
        preview_zone_index++;
        if (preview_zone_index >= preview_zone_count) preview_zone_index = 0;
        preview_zone = preview_zone_list[preview_zone_index];
        preview_offset_y = 0;
        preview_displayed_zone = -1;
        preview_displayed_offset_y = -1;
        update_pic_zone_label();
        request_preview_load();
    } else if (dir == LV_DIR_RIGHT) {
        if (preview_zone_count <= 1) return;
        preview_zone_index--;
        if (preview_zone_index < 0) preview_zone_index = preview_zone_count - 1;
        preview_zone = preview_zone_list[preview_zone_index];
        preview_offset_y = 0;
        preview_displayed_zone = -1;
        preview_displayed_offset_y = -1;
        update_pic_zone_label();
        request_preview_load();
    } else if (dir == LV_DIR_TOP) {
        // Scroll down (see content below)
        if (preview_total_h > VIDEO_H) {
            preview_offset_y += 120;
            if (preview_offset_y > preview_total_h - VIDEO_H) {
                preview_offset_y = preview_total_h - VIDEO_H;
            }
            preview_displayed_offset_y = -1;
            request_preview_load();
        }
    } else if (dir == LV_DIR_BOTTOM) {
        // Scroll up
        if (preview_total_h > VIDEO_H && preview_offset_y > 0) {
            preview_offset_y -= 120;
            if (preview_offset_y < 0) preview_offset_y = 0;
            preview_displayed_offset_y = -1;
            request_preview_load();
        }
    }
}

static void log_upload_ready_paths(const char *clean)
{
    char payload[384];
    snprintf(payload, sizeof(payload), "%s", clean + 13);

    char *fields[6] = {0};
    char *p = payload;
    for (int i = 0; i < 6 && p; i++) {
        fields[i] = p;
        char *sep = strchr(p, '|');
        if (sep) {
            *sep = '\0';
            p = sep + 1;
        } else {
            p = NULL;
        }
    }

    ESP_LOGI(TAG, "Upload ready scan: %s", fields[0] ? fields[0] : "N/A");
    ESP_LOGI(TAG, "Upload photo Z1: %s", fields[1] ? fields[1] : "N/A");
    ESP_LOGI(TAG, "Upload photo Z2: %s", fields[2] ? fields[2] : "N/A");
    ESP_LOGI(TAG, "Upload photo Z3: %s", fields[3] ? fields[3] : "N/A");
    ESP_LOGI(TAG, "Upload photo Bottom: %s", fields[4] ? fields[4] : "N/A");
    ESP_LOGI(TAG, "Model output dir: %s", fields[5] ? fields[5] : "N/A");
}

static void log_model_done_fields(char **fields)
{
    ESP_LOGI(TAG, "Model generated name: %s", fields[0] ? fields[0] : "N/A");
    ESP_LOGI(TAG, "Model generated time: %s", fields[1] ? fields[1] : "N/A");
    ESP_LOGI(TAG, "Model file size: %s", fields[2] ? fields[2] : "N/A");
    ESP_LOGI(TAG, "Pointcloud file size: %s", fields[3] ? fields[3] : "N/A");
    ESP_LOGI(TAG, "Model output dir: %s", fields[4] ? fields[4] : "N/A");
}

static void upload_arc_anim_cb(void *obj, int32_t value)
{
    lv_arc_set_value((lv_obj_t *)obj, value);
    if (upload_percent_label) {
        lv_label_set_text_fmt(upload_percent_label, "%d%%", (int)value);
    }
}

static void show_upload_progress(bool show)
{
    if (!upload_panel || !upload_arc || !upload_percent_label || !upload_stage_label) {
        return;
    }

    if (show) {
        lv_obj_clear_flag(upload_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(upload_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(upload_percent_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(upload_stage_label, LV_OBJ_FLAG_HIDDEN);

        if (label_pic_zone) lv_obj_add_flag(label_pic_zone, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(upload_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(upload_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(upload_percent_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(upload_stage_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_upload_progress_animated(int percent, const char *stage)
{
    if (!upload_arc) {
        return;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    show_upload_progress(true);
    if (upload_stage_label && stage) {
        lv_label_set_text(upload_stage_label, stage);
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, upload_arc);
    lv_anim_set_values(&a, lv_arc_get_value(upload_arc), percent);
    lv_anim_set_time(&a, 350);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, upload_arc_anim_cb);
    lv_anim_start(&a);
}

static void handle_model_progress(const char *clean)
{
    char payload[96];
    snprintf(payload, sizeof(payload), "%s", clean + 15);
    char *sep = strchr(payload, '|');
    if (!sep) {
        return;
    }
    *sep = '\0';
    int percent = atoi(payload);
    const char *stage = sep + 1;
    ESP_LOGI(TAG, "Model progress: %d%% %s", percent, stage);
    set_upload_progress_animated(percent, stage);
}

// ============== K230断开处理 ==============

static void handle_k230_disconnect(const char *reason)
{
    ESP_LOGW(TAG, "K230 disconnected: %s (state was %d)", reason, current_state);
    k230_connected = false;
    if (slider_move_timer) esp_timer_stop(slider_move_timer);
    slider_height_mm = 0;
    slider_moving = false;
    cmd_send_lock = false;
    pending_move_direction = 0;
    pending_cancel_capture = false;
    capture_cancelled = false;

    // 同步停止camera_client的流任务，防止recv()在异常socket上操作导致pbuf崩溃
    k230_client_force_stop_stream();

    request_state_change(STATE_CONN_FAILED);
}

// ============== 状态切换函数 ==============

static void cleanup_camera_page_resources(void)
{
    ESP_LOGI(TAG, "Cleaning up camera page resources");

    k230_connected = false;
    bg_keepalive_mode = false;
    uart_rx_running = false;
    preview_mode = false;
    preview_task_handle = NULL;
    k230_connect_task_handle = NULL;
    uart_rx_task_handle = NULL;

    if (detect_timer) {
        esp_timer_stop(detect_timer);
        esp_timer_delete(detect_timer);
        detect_timer = NULL;
    }
    if (force_stop_timer) {
        esp_timer_stop(force_stop_timer);
        esp_timer_delete(force_stop_timer);
        force_stop_timer = NULL;
    }
    if (slider_move_timer) {
        esp_timer_stop(slider_move_timer);
        esp_timer_delete(slider_move_timer);
        slider_move_timer = NULL;
    }
    if (delayed_disconnect_timer) {
        esp_timer_stop(delayed_disconnect_timer);
        esp_timer_delete(delayed_disconnect_timer);
        delayed_disconnect_timer = NULL;
    }
    if (deferred_timer) {
        lv_timer_del(deferred_timer);
        deferred_timer = NULL;
    }

    k230_client_set_frame_callback(NULL);
    k230_client_force_stop_stream();

    if (video_mutex) {
        vSemaphoreDelete(video_mutex);
        video_mutex = NULL;
    }
    if (lcd_draw_mutex) {
        vSemaphoreDelete(lcd_draw_mutex);
        lcd_draw_mutex = NULL;
    }

    if (screen_camera) {
        lv_obj_del(screen_camera);
        screen_camera = NULL;
    }

    label_status = NULL;
    label_height = NULL;
    btn_back = NULL;
    btn_start = NULL;
    label_start = NULL;
    btn_cancel = NULL;
    btn_up = NULL;
    btn_down = NULL;
    btn_zero = NULL;
    btn_calibrate = NULL;
    label_calibrate = NULL;

    label_pic_zone = NULL;
    upload_panel = NULL;
    upload_arc = NULL;
    upload_percent_label = NULL;
    upload_stage_label = NULL;

    preview_free_cache();
    preview_total_h = 0;
    preview_offset_y = 0;

    slider_height_mm = 0;
    slider_moving = false;
    cmd_send_lock = false;
    pending_move_direction = 0;
    pending_cancel_capture = false;
    capture_cancelled = false;
    has_pending_state = false;
    has_pending_ui_update = false;
    has_pending_label = false;
    current_state = STATE_CONNECTING;
}

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
        set_btn_disabled(btn_zero, COLOR_GRAY);
        set_btn_disabled(btn_cancel, COLOR_GRAY);
        // 校准按钮隐藏
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        break;

    case STATE_CONN_FAILED:
        label_status ? lv_label_set_text(label_status, "Failed") : (void)0;
        lv_label_set_text(label_start, "Reconn");
        set_btn_style(btn_start, COLOR_ORANGE, true);  // 允许重连
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
        set_btn_disabled(btn_cancel, COLOR_GRAY);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        break;

    case STATE_IDLE:
        preview_mode = false;

        if (label_pic_zone) lv_obj_add_flag(label_pic_zone, LV_OBJ_FLAG_HIDDEN);
        preview_free_cache();
        preview_total_h = 0;
        if (label_status) { lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN); lv_label_set_text(label_status, "Ready"); }
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
        set_btn_disabled(btn_zero, COLOR_GRAY);
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        if (detect_timer) {
            esp_timer_start_once(detect_timer, DETECT_TIMEOUT_MS * 1000);  // ms → us
        }
        break;

    case STATE_POSITIONING:
        label_status ? lv_label_set_text(label_status, "Pos...") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
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
        label_status ? lv_label_set_text(label_status, "Pos Fail") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
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

    case STATE_CAPTURING:
        preview_mode = false;

        if (label_pic_zone) lv_obj_add_flag(label_pic_zone, LV_OBJ_FLAG_HIDDEN);
        preview_free_cache();
        preview_total_h = 0;
        label_status ? lv_label_set_text(label_status, "Zone 1...") : (void)0;
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        break;

    case STATE_UPLOAD_READY:
        show_upload_progress(false);
        preview_mode = true;
        preview_zone = 1;
        preview_zone_index = 0;
        preview_offset_y = 0;
        if (label_status) lv_obj_add_flag(label_status, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(label_start, "Upload");
        set_btn_style(btn_start, COLOR_GREEN, true);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);

        // Label hidden until cache download sets the correct zone
        if (label_pic_zone) lv_obj_add_flag(label_pic_zone, LV_OBJ_FLAG_HIDDEN);
        request_preview_load();
        break;

    case STATE_UPLOADING:
        preview_mode = false;
        upload_cancelled = false;  // Fresh upload, reset cancel flag
        // Keep preview cache alive for cancel-back-to-UPLOAD_READY
        preview_total_h = 0;
        set_upload_progress_animated(5, "Preparing");
        label_status ? lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN), lv_label_set_text(label_status, "Upload") : (void)0;
        lv_label_set_text(label_start, "Wait");
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        break;

    case STATE_UPLOAD_FAILED:
        preview_mode = false;
        preview_total_h = 0;
        label_status ? lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN), lv_label_set_text(label_status, "Up Fail") : (void)0;
        lv_label_set_text(label_start, "Upload");
        set_btn_style(btn_start, COLOR_ORANGE, true);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        break;

    case STATE_MODEL_DONE:
        preview_mode = false;
        preview_total_h = 0;
        set_upload_progress_animated(100, "Done");
        label_status ? lv_obj_clear_flag(label_status, LV_OBJ_FLAG_HIDDEN), lv_label_set_text(label_status, "Done") : (void)0;
        lv_label_set_text(label_start, "Done");
        set_btn_disabled(btn_start, COLOR_GRAY);
        set_btn_disabled(btn_up, COLOR_GRAY);
        set_btn_disabled(btn_down, COLOR_GRAY);
        set_btn_disabled(btn_zero, COLOR_GRAY);
        set_btn_style(btn_cancel, COLOR_RED, true);
        if (btn_calibrate) lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);
        break;
    }

    if (locked) xSemaphoreGiveRecursive(lcd_draw_mutex);
}

// ============== 强制停止回调 ==============

static void force_stop_cb(void *arg)
{
    ESP_LOGW(TAG, "Force stop: K230 not responding, forcing to IDLE");
    request_state_change(STATE_IDLE);
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

// ============== 滑块移动超时回调 ==============

static void slider_move_timeout_cb(void *arg)
{
    if (slider_moving) {
        ESP_LOGW(TAG, "Slider move timeout! MOVE:OK not received, resetting slider_moving");
        slider_moving = false;
        cmd_send_lock = false;
        pending_move_direction = 0;
        request_ui_update();
    }
}

// ============== UART消息处理 ==============

static void process_uart_message(const char *clean)
{
    if (strncmp(clean, "FOUND:", 6) == 0) {
        if (current_state == STATE_DETECTING) {
            request_state_change(STATE_POSITIONING);
        }
    } else if (strncmp(clean, "POS:OK", 6) == 0) {
        const char *p = strchr(clean + 6, ':');
        if (p) {
            p++;
            slider_height_mm = atof(p);
            ESP_LOGI(TAG, "POS:OK height=%.1fmm", slider_height_mm);
        }
        if (current_state == STATE_POSITIONING || current_state == STATE_DETECTING) {
            if (detect_timer) esp_timer_stop(detect_timer);
            request_state_change(STATE_POS_SUCCESS);
        }
    } else if (strncmp(clean, "POS:LIMIT", 9) == 0) {
        const char *p = strchr(clean + 9, ':');
        if (p) {
            p++;
            slider_height_mm = atof(p);
            ESP_LOGI(TAG, "POS:LIMIT height=%.1fmm", slider_height_mm);
        }
        if (current_state == STATE_POSITIONING || current_state == STATE_DETECTING) {
            if (detect_timer) esp_timer_stop(detect_timer);
            request_state_change(STATE_LIMIT_FAILED);
        }
    } else if (strncmp(clean, "MOVE:OK", 7) == 0) {
        ui_camera_heartbeat();

        slider_moving = false;
        cmd_send_lock = false;
        pending_move_direction = 0;
        if (slider_move_timer) esp_timer_stop(slider_move_timer);
        const char *h_str = strchr(clean, ':');
        if (h_str) {
            h_str++;
            h_str = strchr(h_str, ':');
            if (h_str) {
                h_str++;
                slider_height_mm = atof(h_str);
            }
        }
        ESP_LOGI(TAG, "MOVE:OK, height=%.1fmm", slider_height_mm);
        request_ui_update();
    } else if (strncmp(clean, "HEIGHT:", 7) == 0) {
        ui_camera_heartbeat();

        if (slider_move_timer) esp_timer_stop(slider_move_timer);
        slider_moving = false;
        cmd_send_lock = false;
        pending_move_direction = 0;
        slider_height_mm = atof(clean + 7);
        ESP_LOGI(TAG, "HEIGHT from K230: %.1fmm", slider_height_mm);
        request_ui_update();

        // 拍摄中Cancel等待: 高度已刷新，现在可以发STOP归零
        if (pending_cancel_capture) {
            pending_cancel_capture = false;
            ESP_LOGI(TAG, "Cancel capture: height refreshed (%.1fmm), sending STOP", slider_height_mm);
            c3_uart_send(CMD_STOP);
            request_state_change(STATE_IDLE);
            if (force_stop_timer) {
                esp_timer_start_once(force_stop_timer, 5000000);
            }
        }
    } else if (strcmp(clean, "ZERO:OK") == 0) {
        ui_camera_heartbeat();

        if (slider_move_timer) esp_timer_stop(slider_move_timer);
        slider_height_mm = 0;
        pending_move_direction = 0;
        slider_moving = false;
        ESP_LOGI(TAG, "ZERO:OK, height reset to 0");
        request_state_change(STATE_IDLE);
    } else if (strcmp(clean, "HOME:OK") == 0) {
        ui_camera_heartbeat();

        if (slider_move_timer) esp_timer_stop(slider_move_timer);
        slider_height_mm = 0;
        pending_move_direction = 0;
        slider_moving = false;
        ESP_LOGI(TAG, "HOME:OK, height reset to 0");
        request_ui_update();
    } else if (strcmp(clean, "STOP:OK") == 0) {

        if (slider_move_timer) esp_timer_stop(slider_move_timer);
        slider_height_mm = 0;
        pending_move_direction = 0;
        slider_moving = false;
        if (force_stop_timer) {
            esp_timer_stop(force_stop_timer);
        }
        if (current_state != STATE_IDLE) {
            ESP_LOGI(TAG, "K230 STOP:OK received, switching to IDLE");
            request_state_change(STATE_IDLE);
        } else {
            ESP_LOGI(TAG, "K230 STOP:OK received, already in IDLE");
            request_ui_update();
        }
    } else if (strcmp(clean, "STATE:DETECTING") == 0) {
        ESP_LOGI(TAG, "K230 confirmed DETECTING state");
        if (current_state != STATE_DETECTING) {
            request_state_change(STATE_DETECTING);
        }
    } else if (strncmp(clean, "CAPTURE:Z", 9) == 0) {
        if (capture_cancelled) {
            ESP_LOGI(TAG, "CAPTURE:Z ignored (capture was cancelled)");
        } else if (pending_cancel_capture) {
            // Cancel等待中，K230已完成移动和拍照，现在发STOP归零
            ESP_LOGI(TAG, "Cancel capture: CAPTURE:Z received, sending STOP");
            pending_cancel_capture = false;
            c3_uart_send(CMD_STOP);
            request_state_change(STATE_IDLE);
            if (force_stop_timer) {
                esp_timer_start_once(force_stop_timer, 5000000);
            }
        } else {
            ui_camera_heartbeat();
            int zone_num = clean[9] - '0';
            if (zone_num >= 1 && zone_num <= 3) {
                if (current_state != STATE_CAPTURING) {
                    request_state_change(STATE_CAPTURING);
                }
                char zone_text[20];
                if (clean[10] == ':' && clean[11] != '\0') {
                    snprintf(zone_text, sizeof(zone_text), "Z%d %s", zone_num, clean + 11);
                } else {
                    snprintf(zone_text, sizeof(zone_text), "Zone %d...", zone_num);
                }
                if (label_status) request_label_update(zone_text);
                ESP_LOGI(TAG, "Capture: %s", zone_text);
            }
        }
    } else if (strncmp(clean, "UPLOAD_READY|", 13) == 0) {
        ui_camera_heartbeat();
        log_upload_ready_paths(clean);
        pending_cancel_capture = false;
        capture_cancelled = false;
        request_label_update("Upload");
        request_state_change(STATE_UPLOAD_READY);
    } else if (strncmp(clean, "UPLOAD_READY", 12) == 0) {
        ui_camera_heartbeat();
        pending_cancel_capture = false;
        capture_cancelled = false;
        request_label_update("Upload");
        request_state_change(STATE_UPLOAD_READY);
    } else if (strncmp(clean, "MODEL_PROGRESS|", 15) == 0) {
        if (upload_cancelled) return;
        ui_camera_heartbeat();
        handle_model_progress(clean);
        request_state_change(STATE_UPLOADING);
    } else if (strcmp(clean, "MODEL_UPLOAD") == 0 || strcmp(clean, "MODEL_WAIT") == 0) {
        if (upload_cancelled) return;
        ui_camera_heartbeat();
        request_state_change(STATE_UPLOADING);
    } else if (strncmp(clean, "MODEL_ERROR", 11) == 0) {
        if (upload_cancelled) return;
        ui_camera_heartbeat();
        request_label_update("Up Fail");
        request_state_change(STATE_UPLOAD_FAILED);
    } else if (strcmp(clean, "ALL_DONE") == 0 || strcmp(clean, "CANCELLED") == 0) {
        ui_camera_heartbeat();
        pending_cancel_capture = false;  // 无论什么情况，清除等待标志
        if (capture_cancelled || strcmp(clean, "CANCELLED") == 0) {
            ESP_LOGI(TAG, "Capture %s, sending HOME to return to zero", clean);
            capture_cancelled = false;
            // ALL_DONE/CANCELLED意味着拍摄线程已结束，直接发HOME归零
            // 注意: 必须从UART线程发HOME（c3_uart_send是线程安全的），
            // 状态切换和UI更新必须通过request函数到LVGL主线程
            slider_moving = true;
            pending_move_direction = -1;
            if (slider_move_timer) esp_timer_start_once(slider_move_timer, 25000000);
            c3_uart_send("HOME");
            request_ui_update();
            request_state_change(STATE_IDLE);
        } else {
            ESP_LOGI(TAG, "All zones captured, switching to UPLOAD_READY");
            request_label_update("Upload");
            request_state_change(STATE_UPLOAD_READY);
        }
    } else if (strncmp(clean, "MODEL_DONE|", 11) == 0) {
        char payload[192];
        snprintf(payload, sizeof(payload), "%s", clean + 11);
        char *fields[5] = {0};
        char *p = payload;
        for (int i = 0; i < 5 && p; i++) {
            fields[i] = p;
            char *sep = strchr(p, '|');
            if (sep) {
                *sep = '\0';
                p = sep + 1;
            } else {
                p = NULL;
            }
        }

        if (fields[0] && fields[1]) {
            log_model_done_fields(fields);
            esp_err_t err = history_store_add_full(fields[0],
                                                   fields[1],
                                                   fields[2] ? fields[2] : "N/A",
                                                   fields[3] ? fields[3] : "N/A",
                                                   fields[4] ? fields[4] : "N/A");
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Model history saved: %s / %s", fields[0], fields[1]);
            } else {
                ESP_LOGE(TAG, "Model history save failed: %s", esp_err_to_name(err));
            }
            set_upload_progress_animated(100, "Done");
            request_state_change(STATE_IDLE);
        } else {
            ESP_LOGW(TAG, "Invalid MODEL_DONE message: %s", clean);
        }
    } else if (strncmp(clean, "MODEL_DONE:", 11) == 0) {
        const char *payload = clean + 11;
        const char *sep = strchr(payload, ':');
        if (sep && sep > payload && *(sep + 1) != '\0') {
            char model_name[HISTORY_MODEL_NAME_LEN];
            char created_time[HISTORY_CREATED_TIME_LEN];
            size_t name_len = sep - payload;
            if (name_len >= sizeof(model_name)) {
                name_len = sizeof(model_name) - 1;
            }
            memcpy(model_name, payload, name_len);
            model_name[name_len] = '\0';
            snprintf(created_time, sizeof(created_time), "%s", sep + 1);

            esp_err_t err = history_store_add(model_name, created_time);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Model history saved: %s / %s", model_name, created_time);
            } else {
                ESP_LOGE(TAG, "Model history save failed: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGW(TAG, "Invalid MODEL_DONE message: %s", clean);
        }
        set_upload_progress_animated(100, "Done");
        request_state_change(STATE_IDLE);
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

            // 按换行符分割多条消息
            char *line_start = rx_buf;
            char *newline = strchr(line_start, '\n');
            while (newline != NULL) {
                *newline = '\0';
                char *clean = line_start;
                while (*clean == '\r') clean++;  // 跳过前导\r
                char *end = clean + strlen(clean) - 1;
                while (end > clean && *end == '\r') { *end = '\0'; end--; }

                if (strlen(clean) > 0) {
                    ESP_LOGI(TAG, "UART RX: [%s]", clean);
                    process_uart_message(clean);  // 处理单条消息
                }

                line_start = newline + 1;
                newline = strchr(line_start, '\n');
            }
            // 处理缓冲区末尾没有换行的残留数据
            if (strlen(line_start) > 0) {
                char *clean = line_start;
                while (*clean == '\r') clean++;
                char *end = clean + strlen(clean) - 1;
                while (end > clean && *end == '\r') { *end = '\0'; end--; }
                if (strlen(clean) > 0) {
                    ESP_LOGI(TAG, "UART RX (partial): [%s]", clean);
                    process_uart_message(clean);
                }
            }
        } else if (len < 0) {
            // UART读取错误
        }

        // 检测K230断开 (视频流超时)
        // 注意: motor_busy时视频流暂停是正常的, 不检测超时
        // 同时, slider_moving为true时也不检测(用户正在调整高度)
        if (k230_connected &&
            !slider_moving &&
            current_state != STATE_CONNECTING &&
            current_state != STATE_CONN_FAILED &&
            current_state != STATE_CALIBRATING &&
            current_state != STATE_DETECTING &&
            current_state != STATE_POSITIONING &&
            current_state != STATE_POS_SUCCESS &&
            current_state != STATE_LIMIT_FAILED &&
            current_state != STATE_POS_FAILED &&
            current_state != STATE_CAPTURING &&
            current_state != STATE_UPLOAD_READY &&
            current_state != STATE_UPLOADING &&
            current_state != STATE_UPLOAD_FAILED &&
            current_state != STATE_MODEL_DONE) {
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
    uint32_t task_session = camera_session_id;
    ESP_LOGI(TAG, "Starting K230 connection...");

    k230_client_config_t k230_config = {
        .host = "192.168.43.13",
        .http_port = 8080,
        .is_connected = false
    };

    esp_err_t err = k230_client_init(&k230_config, progress_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "K230 init failed: %s", esp_err_to_name(err));
        if (camera_session_is_valid(task_session)) {
            request_state_change(STATE_CONN_FAILED);
        }
        finish_k230_connect_task(task_session);
        return;
    }

    k230_client_set_frame_callback(video_frame_callback);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!camera_session_is_valid(task_session)) {
        ESP_LOGI(TAG, "K230 connect task ignored: stale session before connect");
        finish_k230_connect_task(task_session);
        return;
    }

    err = k230_client_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Connection failed: %s", esp_err_to_name(err));
        if (camera_session_is_valid(task_session)) {
            request_state_change(STATE_CONN_FAILED);
        } else {
            ESP_LOGI(TAG, "K230 connect failure ignored: stale session");
        }
        finish_k230_connect_task(task_session);
        return;
    }
    if (!camera_session_is_valid(task_session)) {
        ESP_LOGI(TAG, "K230 connect success ignored: stale session");
        k230_client_force_stop_stream();
        finish_k230_connect_task(task_session);
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
    if (!camera_session_is_valid(task_session)) {
        ESP_LOGI(TAG, "K230 connect task ignored: stale session before stream");
        finish_k230_connect_task(task_session);
        return;
    }

    if (developer_mode_is_upload_test()) {
        ESP_LOGI(TAG, "Developer Upload Test enabled: request K230 test pictures");
        err = k230_client_start_upload_test();
        if (err == ESP_OK) {
            if (camera_session_is_valid(task_session)) {
                first_frame_received = true;
                request_label_update("Upload");
                request_state_change(STATE_UPLOAD_READY);
            }
        } else {
            ESP_LOGE(TAG, "Upload Test request failed: %s", esp_err_to_name(err));
            if (camera_session_is_valid(task_session)) {
                request_state_change(STATE_CONN_FAILED);
            }
        }
        finish_k230_connect_task(task_session);
        return;
    }

    err = k230_client_start_stream();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Stream started");
    } else {
        ESP_LOGE(TAG, "Stream failed: %s", esp_err_to_name(err));
    }

    finish_k230_connect_task(task_session);
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
        request_state_change(STATE_IDLE);
    }

    if (preview_mode) {
        return;
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
        .scale = 1,
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
        // 如果已经连接且有视频流，进入后台保活模式（30秒后才真正断开）
        if (k230_connected && current_state >= STATE_IDLE) {
            ESP_LOGI(TAG, "Back clicked: entering background keepalive mode (30s delay)");
            start_delayed_disconnect();
            return;
        }

        // 未连接或正在连接中，立即断开（不需要保活）
        ESP_LOGI(TAG, "Back clicked: immediate cleanup (not in stream mode)");
        camera_page_active = false;
        camera_session_id++;
        lv_scr_load(ui_home_get_screen());
        cleanup_camera_page_resources();
        return;

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

        // 确保延迟断开定时器也停止（如果存在）
        if (delayed_disconnect_timer) {
            esp_timer_stop(delayed_disconnect_timer);
        }

        // 重置状态并立即切换页面
        current_state = STATE_CONNECTING;
        lv_scr_load(ui_home_get_screen());
    }
}

static void upload_start_task(void *arg)
{
    esp_err_t err = k230_client_start_upload();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Upload start failed: %s", esp_err_to_name(err));
        request_state_change(STATE_UPLOAD_FAILED);
    }
    vTaskDelete(NULL);
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
        capture_cancelled = false;  // 重置取消标志，开始新的扫描
        pending_cancel_capture = false;
        k230_client_start_scan();
    } else if (current_state == STATE_UPLOAD_READY || current_state == STATE_UPLOAD_FAILED) {
        ESP_LOGI(TAG, "Upload: sending UPLOAD to K230");
        switch_state(STATE_UPLOADING);
        xTaskCreate(upload_start_task, "upload", 4096, NULL, 4, NULL);
    } else if (current_state == STATE_CONN_FAILED) {
        ESP_LOGI(TAG, "Reconnect: restarting connection to K230");
        first_frame_received = false;  // 重置首帧标志，允许新连接触发IDLE切换
        // 停止旧的UART任务
        uart_rx_running = false;
        if (uart_rx_task_handle) {
            vTaskDelay(pdMS_TO_TICKS(50));
            uart_rx_task_handle = NULL;
        }
        k230_client_force_stop_stream();
        switch_state(STATE_CONNECTING);
        xTaskCreate(k230_connect_task, "k230_conn", 8192, NULL, 5, &k230_connect_task_handle);
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
        // K230还在YOLO模式，发送STOP让它退出并清理，同时立即切回IDLE
        ESP_LOGI(TAG, "Cancel: sending STOP to K230 and switching to IDLE");
        c3_uart_send(CMD_STOP);
        // 立即恢复IDLE状态，不等待K230响应
        switch_state(STATE_IDLE);
        // 启动强制停止定时器 (5秒后如果K230没响应则确保状态正确)
        if (force_stop_timer) {
            esp_timer_start_once(force_stop_timer, 5000000);
        }
    } else if (current_state == STATE_POS_SUCCESS || current_state == STATE_LIMIT_FAILED ||
               current_state == STATE_POS_FAILED) {
        // K230已退出YOLO，但滑块可能在非零位置，发送HOME归零
        ESP_LOGI(TAG, "Cancel: sending HOME to return slider to zero");
        slider_moving = true;
        pending_move_direction = -1;  // HOME = going down to 0
        update_up_down_buttons();
        if (slider_move_timer) esp_timer_start_once(slider_move_timer, 25000000);  // 25s for HOME
                c3_uart_send("HOME");
        switch_state(STATE_IDLE);
    } else if (current_state == STATE_UPLOAD_READY || current_state == STATE_UPLOADING ||
               current_state == STATE_UPLOAD_FAILED || current_state == STATE_MODEL_DONE) {
        // Cancel upload: go back to preview state (reuse cache if available)
        upload_cancelled = true;
        if (!preview_cache_ready) {
            preview_free_cache();
        }
        switch_state(STATE_UPLOAD_READY);
    } else if (current_state == STATE_CAPTURING) {
        // 拍摄中取消: 不立即发STOP，等当前移动完成(HEIGHT消息)后再发
        // 避免电机中途停下导致高度未刷新，归零不准
        ESP_LOGI(TAG, "Cancel capture: waiting for current move to finish before STOP");
        capture_cancelled = true;  // 标记已取消，忽略后续ALL_DONE
        pending_cancel_capture = true;  // 等HEIGHT/CAPTURE:Z到达后再发STOP
        if (label_status) request_label_update("Cancel...");
    }
}

static void btn_up_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (slider_moving || cmd_send_lock) return;

    // 防抖：距上次发送UP/DOWN不足1.5秒则忽略
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (last_move_send_time_ms > 0 && (now_ms - last_move_send_time_ms) < MIN_MOVE_INTERVAL_MS) {
        ESP_LOGW(TAG, "UP debounced (%lldms < %dms)", now_ms - last_move_send_time_ms, MIN_MOVE_INTERVAL_MS);
        return;
    }

    // 校准模式: 无行程限制
    if (current_state == STATE_CALIBRATING) {
        ESP_LOGI(TAG, "CAL UP (no limit)");
        cmd_send_lock = true;
        slider_moving = true;
        pending_move_direction = 1;
        update_up_down_buttons();
        if (slider_move_timer) esp_timer_start_once(slider_move_timer, 5000000);
        last_move_send_time_ms = now_ms;

        c3_uart_send(CMD_UP);
        return;
    }

    if (current_state == STATE_IDLE || current_state == STATE_POS_SUCCESS ||
        current_state == STATE_LIMIT_FAILED) {
        if (slider_height_mm < SLIDER_UP_LIMIT_MM) {
            ESP_LOGI(TAG, "UP: sending UP to K230 (height=%.1fmm)", slider_height_mm);
            cmd_send_lock = true;
            slider_moving = true;
            pending_move_direction = 1;
            update_up_down_buttons();
            if (slider_move_timer) esp_timer_start_once(slider_move_timer, 5000000);
            last_move_send_time_ms = now_ms;
    
            c3_uart_send(CMD_UP);
        }
    }
}

static void btn_down_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (slider_moving || cmd_send_lock) return;

    // 防抖：距上次发送UP/DOWN不足1.5秒则忽略
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (last_move_send_time_ms > 0 && (now_ms - last_move_send_time_ms) < MIN_MOVE_INTERVAL_MS) {
        ESP_LOGW(TAG, "DOWN debounced (%lldms < %dms)", now_ms - last_move_send_time_ms, MIN_MOVE_INTERVAL_MS);
        return;
    }

    // 校准模式: 无行程限制
    if (current_state == STATE_CALIBRATING) {
        ESP_LOGI(TAG, "CAL DOWN (no limit)");
        cmd_send_lock = true;
        slider_moving = true;
        pending_move_direction = -1;
        update_up_down_buttons();
        if (slider_move_timer) esp_timer_start_once(slider_move_timer, 5000000);
        last_move_send_time_ms = now_ms;
                c3_uart_send(CMD_DOWN);
        return;
    }

    if (current_state == STATE_IDLE || current_state == STATE_POS_SUCCESS ||
        current_state == STATE_LIMIT_FAILED) {
        if (slider_height_mm > 0.0f) {
            ESP_LOGI(TAG, "DOWN: sending DOWN to K230 (height=%.1fmm)", slider_height_mm);
            cmd_send_lock = true;
            slider_moving = true;
            pending_move_direction = -1;
            update_up_down_buttons();
            if (slider_move_timer) esp_timer_start_once(slider_move_timer, 5000000);
            last_move_send_time_ms = now_ms;
                        c3_uart_send(CMD_DOWN);
        }
    }
}

// ============== Zero归零按钮回调 ==============
static void btn_zero_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (slider_moving) return;

    // 校准模式: Zero不可用
    if (current_state == STATE_CALIBRATING) return;

    if (current_state == STATE_IDLE || current_state == STATE_POS_SUCCESS ||
        current_state == STATE_LIMIT_FAILED) {
        if (slider_height_mm > 0.0f) {
            ESP_LOGI(TAG, "ZERO: sending HOME to K230 (height=%.1fmm)", slider_height_mm);
            slider_moving = true;
            pending_move_direction = -1;  // HOME = going down to 0
            update_up_down_buttons();
            if (slider_move_timer) esp_timer_start_once(slider_move_timer, 25000000);  // 25s for HOME
                        c3_uart_send("HOME");
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
    bool resume_from_background = false;
    camera_state_t resume_state = current_state;

    // 如果处于后台保活模式（30秒内点击返回），恢复连接而不是重新创建
    // 注意：必须在删除旧屏幕之前检查，否则会丢失screen_camera引用
    if (ui_camera_is_in_background()) {
        ESP_LOGI(TAG, "Resuming from background keepalive mode...");
        resume_from_background = true;
        resume_state = current_state;
        camera_page_active = true;

        // 先停止延迟断开定时器
        if (delayed_disconnect_timer) {
            esp_timer_stop(delayed_disconnect_timer);
        }

        // 重置后台模式
        bg_keepalive_mode = false;
        ui_home_set_camera_keepalive(false);

        // Upload Test模式：停止视频流，直接进入上传等待状态
        if (developer_mode_is_upload_test()) {
            ESP_LOGI(TAG, "Upload Test resume: stopping stream, using existing cache");
            k230_client_force_stop_stream();
            upload_cancelled = false;  // 重置，允许后续真正上传

            if (screen_camera) {
                lv_scr_load(screen_camera);
                preview_invalidate_display();
            }
            if (!screen_camera) {
                resume_state = STATE_UPLOAD_READY;
            }

            // 直接切换到 UPLOAD_READY，使用已有缓存（不再发 HTTP 请求给 K230）
            first_frame_received = true;
            if (screen_camera) {
                request_label_update("Upload");
                request_state_change(STATE_UPLOAD_READY);
                if (preview_mode && preview_cache_ready) {
                    request_preview_load();
                }
            }
            if (screen_camera) {
                return;
            }
        }

        // 恢复frame callback
        if (!developer_mode_is_upload_test()) {
            k230_client_set_frame_callback(video_frame_callback);
        }

        // 如果已有屏幕，直接加载
        if (screen_camera) {
            lv_scr_load(screen_camera);
            preview_invalidate_display();
            if (preview_mode && preview_cache_ready) {
                request_preview_load();
            }
            return;
        }
        // 如果screen不存在（异常情况），清理旧资源后继续正常创建流程
        ESP_LOGI(TAG, "Camera UI was released in background, recreating UI only");
        // 先清理旧的mutex和定时器，防止重复创建导致内存泄漏
        if (video_mutex) { vSemaphoreDelete(video_mutex); video_mutex = NULL; }
        if (lcd_draw_mutex) { vSemaphoreDelete(lcd_draw_mutex); lcd_draw_mutex = NULL; }
        if (detect_timer) { esp_timer_delete(detect_timer); detect_timer = NULL; }
        if (force_stop_timer) { esp_timer_delete(force_stop_timer); force_stop_timer = NULL; }
    } else {
        // 正常流程：清理旧屏幕
        if (screen_camera) lv_obj_del(screen_camera);
    }

    camera_page_active = true;
    camera_session_id++;

    screen_camera = lv_obj_create(NULL);
    lv_obj_set_size(screen_camera, 240, 320);
    lv_obj_clear_flag(screen_camera, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen_camera, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_camera, LV_OPA_COVER, 0);

    video_mutex = xSemaphoreCreateMutex();
    lcd_draw_mutex = xSemaphoreCreateRecursiveMutex();  // 递归mutex允许同任务重复获取

    // 重置首帧标志 (重新进入页面时必须重新检测)

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

    // 创建滑块移动超时定时器
    esp_timer_create_args_t move_timer_args = {
        .callback = slider_move_timeout_cb,
        .name = "slider_move_timer",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_create(&move_timer_args, &slider_move_timer);

    // 创建LVGL timer用于线程安全延迟执行UI操作 (50ms检查一次)
    deferred_timer = lv_timer_create(deferred_timer_cb, 50, NULL);

    ESP_LOGI(TAG, "Video: %dx%d at (%d,%d)", VIDEO_W, VIDEO_H, VIDEO_POS_X, VIDEO_POS_Y);

    // ===== 返回按钮 - 左上角 =====
    btn_back = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_back, 44, 35);
    lv_obj_set_pos(btn_back, 0, 2);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(COLOR_GRAY_DARK), 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_90, 0);  // 完全不透明，防止黑边
    lv_obj_set_style_radius(btn_back, 6, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_set_style_outline_width(btn_back, 0, 0);
    lv_obj_set_style_shadow_width(btn_back, 0, 0);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, btn_back_callback, LV_EVENT_ALL, NULL);

    // ===== 校准按钮 - 右上角 =====
    btn_calibrate = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_calibrate, 40, 35);
    lv_obj_set_pos(btn_calibrate, 198, 2);
    lv_obj_set_style_bg_color(btn_calibrate, lv_color_hex(COLOR_GREEN), 0);
    lv_obj_set_style_bg_opa(btn_calibrate, LV_OPA_90, 0);  // 完全不透明，防止黑边
    lv_obj_set_style_radius(btn_calibrate, 6, 0);
    lv_obj_set_style_border_width(btn_calibrate, 0, 0);
    lv_obj_set_style_outline_width(btn_calibrate, 0, 0);
    lv_obj_set_style_shadow_width(btn_calibrate, 0, 0);
    lv_obj_add_flag(btn_calibrate, LV_OBJ_FLAG_HIDDEN);

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
    lv_obj_set_style_bg_opa(btn_start, LV_OPA_90, 0);  // 完全不透明，防止黑边
    lv_obj_set_style_radius(btn_start, 8, 0);
    lv_obj_set_style_border_width(btn_start, 0, 0);
    lv_obj_set_style_outline_width(btn_start, 0, 0);
    lv_obj_set_style_shadow_width(btn_start, 0, 0);

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
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_90, 0);  // 完全不透明，防止黑边
    lv_obj_set_style_radius(btn_cancel, 8, 0);
    lv_obj_set_style_border_width(btn_cancel, 0, 0);
    lv_obj_set_style_outline_width(btn_cancel, 0, 0);
    lv_obj_set_style_shadow_width(btn_cancel, 0, 0);

    lv_obj_t *cancel_label = lv_label_create(btn_cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_center(cancel_label);
    lv_obj_add_event_cb(btn_cancel, btn_cancel_callback, LV_EVENT_ALL, NULL);

    // ===== 上移按钮 - 屏幕右侧 (视频右边缘192, 向上移动) =====
    btn_up = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_up, 38, 55);
    lv_obj_set_pos(btn_up, 200, 60);  // 上移
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_bg_opa(btn_up, LV_OPA_90, 0);  // 完全不透明，防止黑边
    lv_obj_set_style_radius(btn_up, 8, 0);
    lv_obj_set_style_border_width(btn_up, 0, 0);
    lv_obj_set_style_outline_width(btn_up, 0, 0);
    lv_obj_set_style_shadow_width(btn_up, 0, 0);

    lv_obj_t *up_label = lv_label_create(btn_up);
    lv_label_set_text(up_label, LV_SYMBOL_UP);
    lv_obj_set_style_text_font(up_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(up_label, lv_color_white(), 0);
    lv_obj_center(up_label);
    lv_obj_add_event_cb(btn_up, btn_up_callback, LV_EVENT_ALL, NULL);

    // ===== Zero归零按钮 - 上下按钮中间，对称位置 =====
    // UP下边缘=115, DOWN上边缘=175, 间隙=60, Zero高度=38
    // Zero上边缘=115+(60-38)/2=126
    btn_zero = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_zero, 38, 38);  // 正方形
    lv_obj_set_pos(btn_zero, 200, 126);
    lv_obj_set_style_bg_color(btn_zero, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_bg_opa(btn_zero, LV_OPA_90, 0);  // 完全不透明，防止黑边
    lv_obj_set_style_radius(btn_zero, 8, 0);
    lv_obj_set_style_border_width(btn_zero, 0, 0);
    lv_obj_set_style_outline_width(btn_zero, 0, 0);
    lv_obj_set_style_shadow_width(btn_zero, 0, 0);

    lv_obj_t *zero_label = lv_label_create(btn_zero);
    lv_label_set_text(zero_label, "Zero");
    lv_obj_set_style_text_font(zero_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(zero_label, lv_color_white(), 0);
    lv_obj_center(zero_label);
    lv_obj_add_event_cb(btn_zero, btn_zero_callback, LV_EVENT_ALL, NULL);

    // ===== 下移按钮 - 屏幕右侧 (视频右边缘192, 向下移动) =====
    btn_down = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_down, 38, 55);
    lv_obj_set_pos(btn_down, 200, 175);  // 下移，与UP对称
    lv_obj_set_style_bg_color(btn_down, lv_color_hex(COLOR_GRAY), 0);
    lv_obj_set_style_bg_opa(btn_down, LV_OPA_90, 0);  // 完全不透明，防止黑边
    lv_obj_set_style_radius(btn_down, 8, 0);
    lv_obj_set_style_border_width(btn_down, 0, 0);
    lv_obj_set_style_outline_width(btn_down, 0, 0);
    lv_obj_set_style_shadow_width(btn_down, 0, 0);

    lv_obj_t *down_label = lv_label_create(btn_down);
    lv_label_set_text(down_label, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_font(down_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(down_label, lv_color_white(), 0);
    lv_obj_center(down_label);
    lv_obj_add_event_cb(btn_down, btn_down_callback, LV_EVENT_ALL, NULL);

    // ===== 高度显示 - 左侧边缘（视频从x=48开始，标签x:0-28有20px安全间隙）=====
    label_height = lv_label_create(screen_camera);
    lv_label_set_text(label_height, "0.0\ncm");
    lv_label_set_long_mode(label_height, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(label_height, 28, 28);  // 紧凑尺寸，与视频保持20px间隙
    lv_obj_set_pos(label_height, 0, 155);
    lv_obj_set_style_text_font(label_height, &lv_font_montserrat_14, 0);
    lv_color_t init_color = lv_color_hex(0xAAAAAA);
    lv_obj_set_style_text_color(label_height, init_color, 0);
    lv_obj_set_style_text_color(label_height, init_color, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(label_height, init_color, LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(label_height, init_color, LV_STATE_CHECKED);
    lv_obj_set_style_text_line_space(label_height, 0, 0);
    lv_obj_set_style_bg_color(label_height, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(label_height, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(label_height, 0, 0);
    lv_obj_set_style_outline_width(label_height, 0, 0);
    lv_obj_set_style_shadow_width(label_height, 0, 0);
    lv_obj_set_style_pad_all(label_height, 0, 0);
    lv_obj_set_style_text_align(label_height, LV_TEXT_ALIGN_CENTER, 0);

    // ===== 状态标签 - 右下角 =====
    label_status = lv_label_create(screen_camera);
    lv_label_set_text(label_status, "Conn...");
    lv_obj_set_style_text_color(label_status, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(label_status, 185, 283);

    // Gesture detection on screen — child gestures bubble UP here (buttons have GESTURE_BUBBLE by default)
    // Do NOT set GESTURE_BUBBLE on screen itself, or gesture will bubble to NULL parent
    lv_obj_add_event_cb(screen_camera, preview_gesture_callback, LV_EVENT_GESTURE, NULL);

    label_pic_zone = lv_label_create(screen_camera);
    lv_label_set_text(label_pic_zone, "Pic Z1");
    lv_obj_set_style_text_color(label_pic_zone, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_pos(label_pic_zone, 185, 283);
    lv_obj_add_flag(label_pic_zone, LV_OBJ_FLAG_HIDDEN);

    upload_panel = lv_obj_create(screen_camera);
    lv_obj_set_size(upload_panel, VIDEO_W, VIDEO_H);
    lv_obj_set_pos(upload_panel, VIDEO_POS_X, VIDEO_POS_Y);
    lv_obj_set_style_bg_color(upload_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(upload_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(upload_panel, 0, 0);
    lv_obj_set_style_radius(upload_panel, 0, 0);
    lv_obj_set_style_shadow_width(upload_panel, 0, 0);
    lv_obj_clear_flag(upload_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(upload_panel, LV_OBJ_FLAG_HIDDEN);

    upload_arc = lv_arc_create(screen_camera);
    lv_obj_set_size(upload_arc, 128, 128);
    lv_obj_set_pos(upload_arc, VIDEO_POS_X + 8, VIDEO_POS_Y + 48);
    lv_arc_set_range(upload_arc, 0, 100);
    lv_arc_set_bg_angles(upload_arc, 0, 360);
    lv_arc_set_rotation(upload_arc, 270);
    lv_arc_set_value(upload_arc, 0);
    lv_obj_set_style_arc_width(upload_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(upload_arc, lv_color_hex(0x2A2A2A), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(upload_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(upload_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(upload_arc, lv_color_hex(0x28A745), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(upload_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(upload_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(upload_arc, 0, LV_PART_KNOB);
    lv_obj_clear_flag(upload_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(upload_arc, LV_OBJ_FLAG_HIDDEN);

    upload_percent_label = lv_label_create(screen_camera);
    lv_label_set_text(upload_percent_label, "0%");
    lv_obj_set_style_text_font(upload_percent_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(upload_percent_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align_to(upload_percent_label, upload_arc, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(upload_percent_label, LV_OBJ_FLAG_HIDDEN);

    upload_stage_label = lv_label_create(screen_camera);
    lv_label_set_text(upload_stage_label, "Preparing");
    lv_obj_set_width(upload_stage_label, VIDEO_W - 12);
    lv_label_set_long_mode(upload_stage_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(upload_stage_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(upload_stage_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_align(upload_stage_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(upload_stage_label, VIDEO_POS_X + 6, VIDEO_POS_Y + 188);
    lv_obj_add_flag(upload_stage_label, LV_OBJ_FLAG_HIDDEN);

    update_height_label();
    lv_scr_load(screen_camera);

    if (resume_from_background && k230_connected) {
        first_frame_received = true;
        switch_state(resume_state);
        if (preview_mode && preview_cache_ready) {
            request_preview_load();
        }
        ESP_LOGI(TAG, "Camera UI recreated from background cache, no reconnect");
        return;
    }

    // 异步连接K230 (不修改原有连接逻辑)
    switch_state(STATE_CONNECTING);
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
            request_state_change(STATE_POSITIONING);
        }
    } else if (strcmp(status_str, "POS:OK") == 0) {
        if (current_state == STATE_POSITIONING) {
            request_state_change(STATE_POS_SUCCESS);
        }
    } else if (strcmp(status_str, "POS:LIMIT") == 0) {
        if (current_state == STATE_POSITIONING) {
            request_state_change(STATE_LIMIT_FAILED);
        }
    } else if (strcmp(status_str, "STOP:OK") == 0) {
        request_state_change(STATE_IDLE);
    } else if (strncmp(status_str, "CAPTURE:Z", 9) == 0) {
        if (capture_cancelled) return;
        if (current_state != STATE_CAPTURING) {
            request_state_change(STATE_CAPTURING);
        }
        int zone_num = status_str[9] - '0';
        if (zone_num >= 1 && zone_num <= 3) {
            char zone_text[16];
            snprintf(zone_text, sizeof(zone_text), "Zone %d...", zone_num);
            if (label_status) request_label_update(zone_text);
        }
    } else if (strcmp(status_str, "ALL_DONE") == 0) {
        if (capture_cancelled) {
            ESP_LOGI(TAG, "ALL_DONE ignored due to previous cancel");
            capture_cancelled = false;
            return;
        }
        request_label_update("Upload");
        request_state_change(STATE_UPLOAD_READY);
    } else if (strncmp(status_str, "UPLOAD_READY", 12) == 0) {
        request_label_update("Upload");
        request_state_change(STATE_UPLOAD_READY);
    } else if (strncmp(status_str, "MODEL_ERROR", 11) == 0) {
        request_label_update("Up Fail");
        request_state_change(STATE_UPLOAD_FAILED);
    }
}

void ui_camera_heartbeat(void)
{
    // 更新最后活动时间，防止超时误判
    // 在收到任何K230数据时调用
    last_frame_time_ms = esp_timer_get_time() / 1000;
}

// ============== 后台保活模式 - 防止误触返回后重新连接等待太久 ==============

/**
 * @brief 延迟断开回调 - 30秒后真正断开K230
 */
static void delayed_disconnect_callback(void *arg)
{
    if (!bg_keepalive_mode) {
        ESP_LOGI(TAG, "Delayed disconnect not needed (already resumed)");
        return;
    }

    ESP_LOGI(TAG, "Delayed disconnect: performing actual K230 disconnect after 30s");

    // 真正断开连接
    bg_keepalive_mode = false;
    request_keepalive_icon_update(false);
    k230_connected = false;

    // 停止UART接收
    uart_rx_running = false;

    // 停止定时器
    if (detect_timer) {
        esp_timer_stop(detect_timer);
    }
    if (force_stop_timer) {
        esp_timer_stop(force_stop_timer);
    }

    // 清空frame callback
    k230_client_set_frame_callback(NULL);

    // 强制断开K230
    k230_client_force_stop_stream();

    // 清理资源
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

    // 重置状态
    current_state = STATE_CONNECTING;

    // 删除Camera屏幕释放LVGL内存
    if (screen_camera) {
        lv_obj_t *old_screen = screen_camera;
        screen_camera = NULL;
        lv_obj_del_async(old_screen);
    }

    if (slider_move_timer) {
        esp_timer_delete(slider_move_timer);
        slider_move_timer = NULL;
    }
    if (deferred_timer) {
        lv_timer_del(deferred_timer);
        deferred_timer = NULL;
    }

    preview_free_cache();
    preview_total_h = 0;
    preview_offset_y = 0;

    ESP_LOGI(TAG, "Delayed disconnect complete");
}

/**
 * @brief 取消延迟断开 - 用户返回相机页面时调用
 */
void ui_camera_cancel_delayed_disconnect(void)
{
    if (!bg_keepalive_mode) {
        // 不在后台模式，无需处理
        return;
    }

    ESP_LOGI(TAG, "Canceling delayed disconnect - user returned to camera");

    // 停止延迟断开定时器
    if (delayed_disconnect_timer) {
        esp_timer_stop(delayed_disconnect_timer);
    }

    // 重置后台模式标志
    bg_keepalive_mode = false;
    ui_home_set_camera_keepalive(false);

    // 恢复frame callback
    k230_client_set_frame_callback(video_frame_callback);

    // 恢复状态检测定时器
    if (detect_timer) {
        esp_timer_start_periodic(detect_timer, 200000);  // 200ms
    }

    ESP_LOGI(TAG, "Resumed from background mode, stream still active");
}

/**
 * @brief 开始延迟断开 - 返回桌面时调用，保持连接30秒
 */
static void start_delayed_disconnect(void)
{
    // 标记后台模式
    bg_keepalive_mode = true;

    ESP_LOGI(TAG, "Starting delayed disconnect: keeping connection for 30s...");

    // 清空frame callback，停止UI更新（但保持连接）
    k230_client_set_frame_callback(NULL);

    // 忽略后续上传相关UART消息，防止 MODEL_ERROR/MODEL_DONE 清空 preview cache
    upload_cancelled = true;

    // 停止状态检测定时器（节省资源）
    if (detect_timer) {
        esp_timer_stop(detect_timer);
    }

    // 创建延迟断开定时器（如果不存在）
    if (!delayed_disconnect_timer) {
        const esp_timer_create_args_t timer_args = {
            .callback = &delayed_disconnect_callback,
            .name = "delayed_disconnect"
        };
        esp_timer_create(&timer_args, &delayed_disconnect_timer);
    }

    // 启动30秒定时器
    esp_timer_start_once(delayed_disconnect_timer, 30000000);  // 30秒 = 30,000,000 us

    // 切换到主页面
    lv_scr_load(ui_home_get_screen());
    ui_home_set_camera_keepalive(true);
    release_camera_screen_for_background();
}

/**
 * @brief 检查是否处于后台保活模式
 */
bool ui_camera_is_in_background(void)
{
    return bg_keepalive_mode;
}
