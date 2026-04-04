/**
 * 设置页面
 */

#include "lvgl.h"
#include "ui_settings.h"
#include "ui_home.h"
#include "ui_brightness.h"
#include "ui_about.h"
#include "ui_wifi.h"
#include "esp_log.h"

static const char *TAG = "ui_settings";
static lv_obj_t *screen_settings = NULL;

static void btn_back_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        lv_scr_load(ui_home_get_screen());
    }
}

static void brightness_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        ui_brightness_create();
    }
}

static void about_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        ui_about_create();
    }
}

static void wifi_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        ui_wifi_create();
    }
}

void ui_settings_create(void)
{
    ESP_LOGI(TAG, "ui_settings_create called, screen_settings pointer: %p", screen_settings);

    // 如果已经创建过，直接加载
    if(screen_settings != NULL) {
        ESP_LOGI(TAG, "Settings screen already exists, loading it");
        lv_scr_load(screen_settings);
        return;
    }

    ESP_LOGI(TAG, "Creating new Settings screen");
    screen_settings = lv_obj_create(NULL);
    lv_obj_set_size(screen_settings, 240, 320);
    lv_obj_set_scrollbar_mode(screen_settings, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_settings, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_settings, LV_OPA_COVER, 0);

    // 返回按钮 (统一样式)
    lv_obj_t *btn_back = lv_btn_create(screen_settings);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, btn_back_callback, LV_EVENT_ALL, NULL);

    // 标题
    lv_obj_t *title = lv_label_create(screen_settings);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    // 菜单列表 - 增加高度和间距
    lv_obj_t *list = lv_list_create(screen_settings);
    lv_obj_set_size(list, 220, 220);
    lv_obj_set_pos(list, 10, 80);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(list, 12, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 8, 0);

    // WiFi选项
    lv_obj_t *btn_wifi = lv_list_add_btn(list, LV_SYMBOL_WIFI, "WiFi");
    lv_obj_set_style_text_font(btn_wifi, &lv_font_montserrat_16, 0);
    lv_obj_set_height(btn_wifi, 55);
    lv_obj_set_style_pad_all(btn_wifi, 12, 0);
    lv_obj_add_event_cb(btn_wifi, wifi_callback, LV_EVENT_ALL, NULL);

    // Brightness选项
    lv_obj_t *btn_brightness = lv_list_add_btn(list, LV_SYMBOL_IMAGE, "Brightness");
    lv_obj_set_style_text_font(btn_brightness, &lv_font_montserrat_16, 0);
    lv_obj_set_height(btn_brightness, 55);
    lv_obj_set_style_pad_all(btn_brightness, 12, 0);
    lv_obj_add_event_cb(btn_brightness, brightness_callback, LV_EVENT_ALL, NULL);

    // About选项
    lv_obj_t *btn_about = lv_list_add_btn(list, LV_SYMBOL_SETTINGS, "About");
    lv_obj_set_style_text_font(btn_about, &lv_font_montserrat_16, 0);
    lv_obj_set_height(btn_about, 55);
    lv_obj_set_style_pad_all(btn_about, 12, 0);
    lv_obj_add_event_cb(btn_about, about_callback, LV_EVENT_ALL, NULL);

    lv_scr_load(screen_settings);
}

lv_obj_t* ui_settings_get_screen(void)
{
    ESP_LOGI(TAG, "ui_settings_get_screen called, returning: %p", screen_settings);
    return screen_settings;
}
