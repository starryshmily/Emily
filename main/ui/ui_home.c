/**
 * 主界面 - 首页 2x2按钮布局 (竖屏 240x320)
 * 带简约拟物化图标和打开动画
 */

#include "lvgl.h"
#include "ui_home.h"
#include "ui_temp.h"
#include "ui_settings.h"
#include "ui_about.h"
#include "ui_camera.h"
#include "gxhtc3.h"
#include "../c3_uart.h"
#include <stdio.h>

// External functions from main.c
extern void app_main_set_wifi_status_label(lv_obj_t *label);
extern bool app_main_is_wifi_connected(void);

static lv_obj_t *screen_home = NULL;

// 动画状态
static lv_obj_t *btn_camera = NULL;
static lv_obj_t *btn_temp = NULL;
static lv_obj_t *btn_xiaozhi = NULL;
static lv_obj_t *btn_settings = NULL;
static float pre_measured_temp = 25.0f;
static bool is_animating = false;

// 动画遮罩对象
static lv_obj_t *anim_mask = NULL;
static lv_obj_t *anim_mask_icon = NULL;
static lv_obj_t *anim_mask_label = NULL;
static int target_page = 0;  // 0=none, 1=camera, 2=temp, 3=xiaozhi, 4=settings

// 保存按钮原始位置和大小
static int32_t anim_start_x = 0;
static int32_t anim_start_y = 0;
static int32_t anim_start_w = 90;
static int32_t anim_start_h = 90;

// WiFi状态指示器
static lv_obj_t *wifi_status_label = NULL;

// 动画执行回调 - 改变遮罩大小和位置 (250ms优化)
static void anim_expand_cb(void *var, int32_t v)
{
    lv_obj_t *mask = (lv_obj_t *)var;

    // v从0到500，计算进度
    int32_t progress = v * 100 / 500;  // 0-100

    // 预计算delta值
    int32_t delta_w = ((240 - anim_start_w) * progress) / 100;
    int32_t delta_h = ((320 - anim_start_h) * progress) / 100;
    int32_t delta_x = (-anim_start_x * progress) / 100;
    int32_t delta_y = (-anim_start_y * progress) / 100;

    lv_obj_set_size(mask, anim_start_w + delta_w, anim_start_h + delta_h);
    lv_obj_set_pos(mask, anim_start_x + delta_x, anim_start_y + delta_y);

    // 圆角渐变
    lv_obj_set_style_radius(mask, 18 - (progress * 18) / 100, 0);

    // 透明度渐变 - 仅在最后阶段更新
    if(progress >= 50) {
        int32_t opa = ((progress - 50) * 255) / 50;
        if(anim_mask_icon) lv_obj_set_style_opa(anim_mask_icon, 255 - opa, 0);
        if(anim_mask_label) lv_obj_set_style_opa(anim_mask_label, 255 - opa, 0);
    }
}

// 动画完成回调 - 加载目标页面
static void anim_ready_cb(lv_anim_t *a)
{
    // 对于Temp按钮，在动画完成后测量温度（不阻塞动画开始）
    if(target_page == 2) {
        gxhtc3_get_tah();
        extern float temp;
        pre_measured_temp = temp;
        if(pre_measured_temp < 0) pre_measured_temp = 0;
        if(pre_measured_temp > 50) pre_measured_temp = 50;
    }

    // 删除遮罩
    if(anim_mask) {
        lv_obj_del(anim_mask);
        anim_mask = NULL;
    }
    anim_mask_icon = NULL;
    anim_mask_label = NULL;

    // 加载目标页面
    switch(target_page) {
        case 1:  // Camera
            ui_camera_create();
            break;
        case 2:  // Temp
            ui_temp_create_with_temp(pre_measured_temp);
            break;
        case 3:  // XiaoZhi
        {
            // 发送UART通知到K230，蜂鸣器响1秒
            c3_uart_send_xiaozhi();
            static const char *btns[] = {"OK", ""};
            lv_msgbox_create(NULL, "XiaoZhi AI", "Notification sent!\nK230 buzzer beeping...", btns, true);
            break;
        }
        case 4:  // Settings
            ui_settings_create();
            break;
    }

    target_page = 0;
    is_animating = false;
}

// 启动按钮放大动画
static void start_button_animation(lv_obj_t *btn, int page_id)
{
    if(is_animating) return;
    is_animating = true;
    target_page = page_id;

    // 保存按钮的原始位置
    anim_start_x = lv_obj_get_x(btn);
    anim_start_y = lv_obj_get_y(btn);
    anim_start_w = lv_obj_get_width(btn);
    anim_start_h = lv_obj_get_height(btn);

    // 获取按钮的样式
    lv_color_t btn_bg = lv_obj_get_style_bg_color(btn, 0);
    lv_color_t btn_grad = lv_obj_get_style_bg_grad_color(btn, 0);

    // 获取按钮内的图标和文字
    lv_obj_t *btn_icon = lv_obj_get_child(btn, 0);
    lv_obj_t *btn_label = lv_obj_get_child(btn, 1);
    const char *icon_text = btn_icon && lv_obj_check_type(btn_icon, &lv_label_class) ? lv_label_get_text(btn_icon) : "";
    const char *label_text = btn_label && lv_obj_check_type(btn_label, &lv_label_class) ? lv_label_get_text(btn_label) : "";

    // 创建遮罩层 - 复制按钮的样式，放在按钮原始位置
    anim_mask = lv_obj_create(lv_scr_act());
    lv_obj_set_size(anim_mask, anim_start_w, anim_start_h);
    lv_obj_set_pos(anim_mask, anim_start_x, anim_start_y);
    lv_obj_set_style_bg_color(anim_mask, btn_bg, 0);
    lv_obj_set_style_bg_grad_color(anim_mask, btn_grad, 0);
    lv_obj_set_style_bg_grad_dir(anim_mask, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(anim_mask, 18, 0);
    lv_obj_set_style_border_width(anim_mask, 0, 0);

    // 在遮罩上复制图标
    if(btn_icon && lv_obj_check_type(btn_icon, &lv_label_class)) {
        anim_mask_icon = lv_label_create(anim_mask);
        lv_label_set_text(anim_mask_icon, icon_text);
        lv_obj_set_style_text_color(anim_mask_icon, lv_color_white(), 0);
        lv_obj_set_style_text_font(anim_mask_icon, &lv_font_montserrat_48, 0);
        lv_obj_align(anim_mask_icon, LV_ALIGN_TOP_MID, 0, 5);
    }

    // 在遮罩上复制文字
    if(btn_label && lv_obj_check_type(btn_label, &lv_label_class)) {
        anim_mask_label = lv_label_create(anim_mask);
        lv_label_set_text(anim_mask_label, label_text);
        lv_obj_set_style_text_color(anim_mask_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(anim_mask_label, &lv_font_montserrat_12, 0);
        lv_obj_align(anim_mask_label, LV_ALIGN_BOTTOM_MID, 0, -5);
    }

    // 将遮罩移到最上层
    lv_obj_move_foreground(anim_mask);

    // 设置动画参数 - 250ms总时间
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, anim_mask);
    lv_anim_set_time(&a, 250);  // 动画时间250ms
    lv_anim_set_delay(&a, 0);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);  // 缓动效果更流畅
    lv_anim_set_exec_cb(&a, anim_expand_cb);
    lv_anim_set_values(&a, 0, 500);  // 500档位
    lv_anim_set_ready_cb(&a, anim_ready_cb);
    lv_anim_start(&a);
}

static void btn_camera_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        start_button_animation(btn_camera, 1);
    }
}

static void btn_temp_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        start_button_animation(btn_temp, 2);
    }
}

static void btn_xiaozhi_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        start_button_animation(btn_xiaozhi, 3);
    }
}

static void btn_settings_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        start_button_animation(btn_settings, 4);
    }
}

static lv_obj_t* create_menu_btn(lv_obj_t *parent, const char *icon_text, const char *label_text, int col, int row, lv_color_t bg_color)
{
    lv_obj_t *btn = lv_btn_create(parent);
    // 按钮大小: 90x90
    // 列位置: 20 (第1列), 130 (第2列)
    // 行位置: 50 (第1行), 180 (第2行)
    lv_obj_set_size(btn, 90, 90);
    lv_obj_set_pos(btn, 20 + col * 110, 50 + row * 130);

    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_grad_color(btn, lv_color_lighten(bg_color, 50), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_shadow_width(btn, 10, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 5, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(bg_color, 30), LV_STATE_PRESSED);

    // 图标 (上方) - 使用48号大字体
    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, icon_text);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 5);

    // 文字标签 (下方)
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);

    return btn;
}

// 创建Temp按钮 - 温度计图标（参照图片样式）
static lv_obj_t* create_temp_btn(lv_obj_t *parent, const char *label_text, int col, int row, lv_color_t bg_color)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 90, 90);
    lv_obj_set_pos(btn, 20 + col * 110, 50 + row * 130);

    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_grad_color(btn, lv_color_lighten(bg_color, 50), 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_shadow_width(btn, 10, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 5, 0);
    lv_obj_set_style_bg_color(btn, lv_color_darken(bg_color, 30), LV_STATE_PRESSED);

    // 创建温度计图标容器
    lv_obj_t *icon_cont = lv_obj_create(btn);
    lv_obj_set_size(icon_cont, 50, 50);
    lv_obj_align(icon_cont, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_set_style_bg_opa(icon_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon_cont, 0, 0);
    lv_obj_set_style_pad_all(icon_cont, 0, 0);
    lv_obj_clear_flag(icon_cont, LV_OBJ_FLAG_CLICKABLE);

    // 温度计管身 (白色玻璃管，带深棕色边框) - 先创建作为底层
    lv_obj_t *stem = lv_obj_create(icon_cont);
    lv_obj_set_size(stem, 10, 36);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_bg_color(stem, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stem, 5, 0);
    lv_obj_set_style_border_width(stem, 2, 0);
    lv_obj_set_style_border_color(stem, lv_color_hex(0x5D4037), 0);
    lv_obj_set_style_border_opa(stem, LV_OPA_COVER, 0);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_CLICKABLE);

    // 管身中部红色液体 (一半多高度)
    lv_obj_t *liquid = lv_obj_create(icon_cont);
    lv_obj_set_size(liquid, 4, 20);
    lv_obj_align(liquid, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_color(liquid, lv_color_hex(0xFF5252), 0);
    lv_obj_set_style_bg_opa(liquid, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(liquid, 2, 0);
    lv_obj_set_style_border_width(liquid, 0, 0);
    lv_obj_clear_flag(liquid, LV_OBJ_FLAG_CLICKABLE);

    // 管身内的刻度线 (5条银色水平线，只有左边一半长度)
    for(int i = 0; i < 5; i++) {
        lv_obj_t *tick = lv_obj_create(icon_cont);
        lv_obj_set_size(tick, 3, 2);  // 只有左边一半长度
        lv_obj_align(tick, LV_ALIGN_CENTER, -2, -14 + i * 6);  // x偏左
        lv_obj_set_style_bg_color(tick, lv_color_hex(0xC0C0C0), 0);  // 银色
        lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(tick, 0, 0);
        lv_obj_set_style_border_width(tick, 0, 0);
        lv_obj_clear_flag(tick, LV_OBJ_FLAG_CLICKABLE);
    }

    // 底部红色球泡
    lv_obj_t *bulb = lv_obj_create(icon_cont);
    lv_obj_set_size(bulb, 16, 16);
    lv_obj_align(bulb, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_bg_color(bulb, lv_color_hex(0xFF5252), 0);
    lv_obj_set_style_bg_opa(bulb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bulb, 8, 0);
    lv_obj_set_style_border_width(bulb, 2, 0);
    lv_obj_set_style_border_color(bulb, lv_color_hex(0x5D4037), 0);
    lv_obj_set_style_border_opa(bulb, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bulb, LV_OBJ_FLAG_CLICKABLE);

    // 文字标签 (下方)
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -5);

    return btn;
}

void ui_home_create(void)
{
    screen_home = lv_obj_create(NULL);
    lv_obj_set_size(screen_home, 240, 320);
    lv_obj_set_scrollbar_mode(screen_home, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_home, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_home, LV_OPA_COVER, 0);

    // 标题
    lv_obj_t *title = lv_label_create(screen_home);
    lv_label_set_text(title, "Smart Control");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    // WiFi状态指示器 (右上角，与标题同高) - 初始隐藏，只在连接时显示
    wifi_status_label = lv_label_create(screen_home);
    lv_label_set_text(wifi_status_label, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_status_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x4CAF50), 0);  // 绿色表示已连接
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_RIGHT, -5, 12);
    lv_obj_add_flag(wifi_status_label, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏

    // Register WiFi status label with main app
    app_main_set_wifi_status_label(wifi_status_label);

    // 创建四个按钮 (90x90, col1=20, col2=130, row1=50, row2=180)
    // Camera - 使用VIDEO符号(录像机样式)
    btn_camera = create_menu_btn(screen_home, LV_SYMBOL_VIDEO, "Camera", 0, 0, lv_color_hex(0xFF5722));
    lv_obj_add_event_cb(btn_camera, btn_camera_callback, LV_EVENT_ALL, NULL);

    // Temp - 使用自定义温度计图标 (Teal青色背景)
    btn_temp = create_temp_btn(screen_home, "Temp", 1, 0, lv_color_hex(0x00BCD4));
    lv_obj_add_event_cb(btn_temp, btn_temp_callback, LV_EVENT_ALL, NULL);

    // XiaoZhi - AI文字图标
    btn_xiaozhi = create_menu_btn(screen_home, "AI", "XiaoZhi", 0, 1, lv_color_hex(0x9C27B0));
    lv_obj_add_event_cb(btn_xiaozhi, btn_xiaozhi_callback, LV_EVENT_ALL, NULL);

    // Setup - 齿轮图标 (灰色)
    btn_settings = create_menu_btn(screen_home, LV_SYMBOL_SETTINGS, "Setup", 1, 1, lv_color_hex(0x78909C));
    lv_obj_add_event_cb(btn_settings, btn_settings_callback, LV_EVENT_ALL, NULL);

    lv_scr_load(screen_home);
}

lv_obj_t* ui_home_get_screen(void)
{
    return screen_home;
}

// 设置WiFi状态 (connected=true表示已连接, false表示未连接)
void ui_home_set_wifi_status(bool connected)
{
    if(wifi_status_label == NULL) return;

    if(connected) {
        // 已连接 - 显示WiFi信号图标
        lv_label_set_text(wifi_status_label, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x4CAF50), 0);  // 绿色表示已连接
    } else {
        // 未连接 - 显示WiFi图标带X
        lv_label_set_text(wifi_status_label, LV_SYMBOL_CLOSE);
        lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x9E9E9E), 0);  // 灰色表示未连接
    }
}
