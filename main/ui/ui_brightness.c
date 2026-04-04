/**
 * 亮度页面 - 带PWM背光控制
 */

#include "lvgl.h"
#include "ui_brightness.h"
#include "ui_settings.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include <stdio.h>

static lv_obj_t *screen_brightness = NULL;
static lv_obj_t *slider = NULL;
static lv_obj_t *label_value = NULL;
static uint32_t brightness_level = 80;

// 背光GPIO
#define BACKLIGHT_GPIO 2

// LEDC配置
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_10_BIT
#define LEDC_FREQUENCY 5000

static void configure_ledc(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode = LEDC_MODE,
        .timer_num = LEDC_TIMER,
        .duty_resolution = LEDC_DUTY_RES,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_MODE,
        .channel = LEDC_CHANNEL,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BACKLIGHT_GPIO,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&ledc_channel);
}

static void set_backlight(uint32_t brightness_percent)
{
    // 背光是active-low，反转占空比
    uint32_t duty = 1023 - (brightness_percent * 1023) / 100;
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

static void btn_back_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        lv_scr_load(ui_settings_get_screen());
    }
}

static void slider_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_VALUE_CHANGED) {
        brightness_level = lv_slider_get_value(slider);

        char buf[16];
        snprintf(buf, sizeof(buf), "%d%%", (int)brightness_level);
        lv_label_set_text(label_value, buf);

        set_backlight(brightness_level);
    }
}

void ui_brightness_create(void)
{
    // 删除旧的screen（如果存在），防止内存泄漏
    if(screen_brightness != NULL) {
        lv_obj_del(screen_brightness);
        screen_brightness = NULL;
        slider = NULL;
        label_value = NULL;
    }

    configure_ledc();
    set_backlight(brightness_level);

    screen_brightness = lv_obj_create(NULL);
    lv_obj_set_size(screen_brightness, 240, 320);
    lv_obj_set_scrollbar_mode(screen_brightness, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_brightness, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(screen_brightness, LV_OPA_COVER, 0);

    // 返回按钮 (统一样式)
    lv_obj_t *btn_back = lv_btn_create(screen_brightness);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0xFF9800), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, btn_back_callback, LV_EVENT_ALL, NULL);

    // 标题
    lv_obj_t *title = lv_label_create(screen_brightness);
    lv_label_set_text(title, "Brightness");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF9800), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    // 滑块（居中）
    slider = lv_slider_create(screen_brightness);
    lv_obj_set_size(slider, 200, 15);
    lv_obj_align(slider, LV_ALIGN_CENTER, 0, -20);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, brightness_level, LV_ANIM_ON);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFF9800), LV_PART_KNOB);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xFFCC80), LV_PART_INDICATOR);
    lv_obj_add_event_cb(slider, slider_callback, LV_EVENT_ALL, NULL);

    // 当前亮度值（滑块下方，居中）
    label_value = lv_label_create(screen_brightness);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", (int)brightness_level);
    lv_label_set_text(label_value, buf);
    lv_obj_set_style_text_font(label_value, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_value, lv_color_hex(0xFF9800), 0);
    lv_obj_align(label_value, LV_ALIGN_CENTER, 0, 40);

    lv_scr_load(screen_brightness);
}
