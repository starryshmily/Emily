/**
 * 设置页面
 */

#include "lvgl.h"
#include "ui_settings.h"
#include "ui_home.h"
#include "ui_brightness.h"
#include "ui_about.h"
#include "ui_wifi.h"
#include "ui_developer.h"
#include "ui_light.h"
#include "settings_store.h"
#include "developer_mode.h"
#include "c3_uart.h"
#include "esp_log.h"

static const char *TAG = "ui_settings";
static lv_obj_t *screen_settings = NULL;
static lv_obj_t *screen_camera = NULL;

static void btn_back_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        ui_home_refresh_developer_status();
        lv_scr_load(ui_home_get_screen());
        ui_home_apply_light_color();
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

static void developer_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        ui_developer_create();
    }
}

static void light_callback(lv_event_t *e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_light_create();
    }
}

static void camera_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_settings_camera_create();
    }
}

static void camera_back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (screen_settings) {
            lv_scr_load(screen_settings);
        } else {
            ui_settings_create();
        }
    }
}

static void bottom_cam_switch_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        settings_store_set_bottom_cam_enabled(enabled);
        c3_uart_send(enabled ? "CAMERA_MODE:3" : "CAMERA_MODE:4");
        ESP_LOGI(TAG, "BOTTOM cam: %s → %s", enabled ? "ON" : "OFF",
                 enabled ? "3-zone+bottom" : "4-zone");
    }
}

static void demo_mode_switch_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        if (enabled) {
            // Auto-disable conflicting developer modes
            if (developer_mode_is_camera_test()) {
                developer_mode_set_camera_test(false);
                c3_uart_send("BOTTOM_CAM:OFF");
            }
            if (developer_mode_is_upload_test()) {
                developer_mode_set_upload_test(false);
            }
        }
        developer_mode_set_progress_bar_test(enabled);
        ESP_LOGI(TAG, "Demo Mode: %s", enabled ? "ON" : "OFF");
    }
}

void ui_settings_camera_create(void)
{
    settings_store_init();
    if (screen_camera) {
        lv_obj_del(screen_camera);
        screen_camera = NULL;
    }

    screen_camera = lv_obj_create(NULL);
    lv_obj_set_size(screen_camera, 240, 320);
    lv_obj_set_scrollbar_mode(screen_camera, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_camera, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_camera, LV_OPA_COVER, 0);

    // Back button
    lv_obj_t *btn_back = lv_btn_create(screen_camera);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, camera_back_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);

    // Title
    lv_obj_t *title = lv_label_create(screen_camera);
    lv_label_set_text(title, "Camera");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    // BOTTOM switch row
    lv_obj_t *row_bottom = lv_obj_create(screen_camera);
    lv_obj_set_size(row_bottom, 208, 52);
    lv_obj_align(row_bottom, LV_ALIGN_TOP_MID, 0, 95);
    lv_obj_set_style_bg_color(row_bottom, lv_color_white(), 0);
    lv_obj_set_style_radius(row_bottom, 8, 0);
    lv_obj_set_style_border_width(row_bottom, 0, 0);
    lv_obj_set_style_pad_all(row_bottom, 8, 0);
    lv_obj_clear_flag(row_bottom, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bottom_label = lv_label_create(row_bottom);
    lv_label_set_text(bottom_label, "BOTTOM");
    lv_obj_set_style_text_font(bottom_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bottom_label, lv_color_hex(0x1F2937), 0);
    lv_obj_align(bottom_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row_bottom);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (settings_store_is_bottom_cam_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, bottom_cam_switch_callback, LV_EVENT_VALUE_CHANGED, NULL);

    // DEMO MODE switch row
    lv_obj_t *row_demo = lv_obj_create(screen_camera);
    lv_obj_set_size(row_demo, 208, 52);
    lv_obj_align(row_demo, LV_ALIGN_TOP_MID, 0, 155);
    lv_obj_set_style_bg_color(row_demo, lv_color_white(), 0);
    lv_obj_set_style_radius(row_demo, 8, 0);
    lv_obj_set_style_border_width(row_demo, 0, 0);
    lv_obj_set_style_pad_all(row_demo, 8, 0);
    lv_obj_clear_flag(row_demo, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *demo_label = lv_label_create(row_demo);
    lv_label_set_text(demo_label, "Demo Mode");
    lv_obj_set_style_text_font(demo_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(demo_label, lv_color_hex(0x1F2937), 0);
    lv_obj_align(demo_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw_demo = lv_switch_create(row_demo);
    lv_obj_align(sw_demo, LV_ALIGN_RIGHT_MID, 0, 0);
    if (developer_mode_is_progress_bar_test()) {
        lv_obj_add_state(sw_demo, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw_demo, demo_mode_switch_callback, LV_EVENT_VALUE_CHANGED, NULL);

    lv_scr_load(screen_camera);
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
    lv_obj_set_size(list, 220, 240);
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
    lv_obj_t *btn_brightness = lv_list_add_btn(list, LV_SYMBOL_TINT, "Brightness");
    lv_obj_set_style_text_font(btn_brightness, &lv_font_montserrat_16, 0);
    lv_obj_set_height(btn_brightness, 55);
    lv_obj_set_style_pad_all(btn_brightness, 12, 0);
    lv_obj_add_event_cb(btn_brightness, brightness_callback, LV_EVENT_ALL, NULL);

    lv_obj_t *btn_light = lv_list_add_btn(list, LV_SYMBOL_CHARGE, "Light");
    lv_obj_set_style_text_font(btn_light, &lv_font_montserrat_16, 0);
    lv_obj_set_height(btn_light, 55);
    lv_obj_set_style_pad_all(btn_light, 12, 0);
    lv_obj_add_event_cb(btn_light, light_callback, LV_EVENT_ALL, NULL);

    lv_obj_t *btn_camera = lv_list_add_btn(list, LV_SYMBOL_EYE_OPEN, "Camera");
    lv_obj_set_style_text_font(btn_camera, &lv_font_montserrat_16, 0);
    lv_obj_set_height(btn_camera, 55);
    lv_obj_set_style_pad_all(btn_camera, 12, 0);
    lv_obj_add_event_cb(btn_camera, camera_callback, LV_EVENT_ALL, NULL);

    lv_obj_t *btn_developer = lv_list_add_btn(list, LV_SYMBOL_SETTINGS, "Developer Mode");
    lv_obj_set_style_text_font(btn_developer, &lv_font_montserrat_16, 0);
    lv_obj_set_height(btn_developer, 55);
    lv_obj_set_style_pad_all(btn_developer, 12, 0);
    lv_obj_add_event_cb(btn_developer, developer_callback, LV_EVENT_ALL, NULL);

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
