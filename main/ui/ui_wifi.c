/**
 * WiFi page - Real WiFi scanning with optimized memory usage
 */

#include "lvgl.h"
#include "ui_wifi.h"
#include "ui_settings.h"
#include "wifi_scanner.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include <string.h>
#include <stdio.h>

// External functions from main.c
extern void app_main_set_wifi_status_label(lv_obj_t *label);
extern bool app_main_is_wifi_connected(void);
extern int get_last_disconnect_reason(void);
extern void app_main_get_connected_ssid(char *ssid, size_t max_len);
extern void app_main_wifi_disconnect(void);
extern void app_main_trigger_wifi_connect(void);

static const char *TAG = "ui_wifi";

// Global screen objects
static lv_obj_t *screen_wifi = NULL;
static lv_obj_t *screen_keyboard = NULL;

// WiFi list components
static lv_obj_t *list_wifi = NULL;
static lv_obj_t *label_status = NULL;
static lv_obj_t *msgbox = NULL;
static lv_timer_t *refresh_timer = NULL;
static lv_timer_t *connect_timer = NULL;
static lv_timer_t *scan_timer = NULL;  // Track scan timer

// State variables
static char selected_ssid[33] = {0};
static bool is_connecting = false;
static bool is_returning = false;
static bool wifi_initialized = false;
static bool scan_in_progress = false;  // Track if scan is running
static char connected_ssid[33] = {0};  // Currently connected SSID

// WiFi AP data - show only top 10 to save memory
#define MAX_AP_COUNT 10
#define MAX_SCAN_RESULTS 30  // Scan more, then filter and sort
typedef struct {
    char ssid[33];
    int8_t rssi;
    lv_obj_t *btn;
} wifi_ap_t;

static wifi_ap_t ap_list[MAX_AP_COUNT];
static int ap_count = 0;

// Keyboard components
static lv_obj_t *keyboard = NULL;
static lv_obj_t *textarea_pwd = NULL;

// Animation variables for keyboard slide-in
static lv_obj_t *anim_mask = NULL;           // Mask covering screen
static lv_obj_t *anim_keyboard_content = NULL;  // Keyboard content on mask
static lv_obj_t *anim_back_btn = NULL;

// Forward declarations
static void wifi_back_cb(lv_event_t *e);
static void wifi_list_click_cb(lv_event_t *e);
static void refresh_btn_cb(lv_event_t *e);
static void keyboard_back_cb(lv_event_t *e);
static void keyboard_connect_cb(lv_event_t *e);
static void keyboard_anim_ready_cb(lv_anim_t *a);
static void connect_timeout_cb(lv_timer_t *t);
static void msgbox_close_cb(lv_event_t *e);
static void do_wifi_scan(void);
static void do_wifi_scan_timer(lv_timer_t *t);
static void disconnect_confirm_cb(lv_event_t *e);

// Animation callback for button zoom
static void btn_zoom_anim_cb(void *obj, int32_t value)
{
    lv_obj_set_style_transform_zoom((lv_obj_t *)obj, value, 0);
}

// Refresh button callback - do WiFi scan and show results
static void refresh_btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        // Don't refresh if already scanning
        if(scan_in_progress) return;

        ESP_LOGI(TAG, "Refresh button clicked - starting WiFi scan");

        // Add press animation
        lv_obj_t *btn = lv_event_get_target(e);
        if(btn) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, btn);
            lv_anim_set_time(&a, 100);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_values(&a, 256, 280);  // Scale 256->280
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)btn_zoom_anim_cb);
            lv_anim_set_playback_time(&a, 100);
            lv_anim_start(&a);
        }

        scan_in_progress = true;

        // Delete any existing scan timer
        if(scan_timer) {
            lv_timer_del(scan_timer);
            scan_timer = NULL;
        }

        // Update status to scanning
        if(label_status) {
            lv_label_set_text(label_status, "Scanning...");
            lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_BLUE), 0);
        }

        // Clear list immediately
        if(list_wifi) {
            lv_obj_clean(list_wifi);
            lv_obj_clear_flag(list_wifi, LV_OBJ_FLAG_SCROLL_CHAIN);
            lv_obj_add_flag(list_wifi, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(list_wifi, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_scrollbar_mode(list_wifi, LV_SCROLLBAR_MODE_AUTO);
        }

        // Reset button pointers
        for(int i = 0; i < MAX_AP_COUNT; i++) {
            ap_list[i].btn = NULL;
        }
        ap_count = 0;

        // Delay scan to allow button animation to complete
        scan_timer = lv_timer_create(do_wifi_scan_timer, 200, NULL);
        lv_timer_set_repeat_count(scan_timer, 1);  // Run only once
    }
}

// Timer callback to perform WiFi scan
static void do_wifi_scan_timer(lv_timer_t *t)
{
    scan_timer = NULL;  // Clear timer pointer
    do_wifi_scan();
}

// Perform WiFi scan and display results
static void do_wifi_scan(void)
{
    scan_in_progress = true;

    ESP_LOGI(TAG, "Starting WiFi scan...");

    // Clear list
    if(list_wifi) {
        lv_obj_clean(list_wifi);
        lv_obj_clear_flag(list_wifi, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_flag(list_wifi, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(list_wifi, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_scrollbar_mode(list_wifi, LV_SCROLLBAR_MODE_AUTO);
    }

    // Reset button pointers
    for(int i = 0; i < MAX_AP_COUNT; i++) {
        ap_list[i].btn = NULL;
    }
    ap_count = 0;

    // 获取当前WiFi状态
    wifi_ap_record_t ap_info;
    bool was_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);

    // 直接扫描，不等待不重试
    ESP_LOGI(TAG, "Starting scan directly (connected=%d)", was_connected);
    esp_err_t ret = wifi_scanner_start_scan();

    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(ret));
        scan_in_progress = false;

        if(label_status) {
            lv_label_set_text(label_status, "Scan failed");
            lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_RED), 0);
        }
        return;
    }

    ESP_LOGI(TAG, "WiFi scan completed successfully");

    // Get currently connected SSID
    app_main_get_connected_ssid(connected_ssid, sizeof(connected_ssid));
    ESP_LOGI(TAG, "Currently connected to: %s", connected_ssid[0] ? connected_ssid : "(none)");

    // Get scan results
    wifi_ap_info_t results[MAX_AP_COUNT];
    int found = wifi_scanner_get_results(results, MAX_AP_COUNT);
    ESP_LOGI(TAG, "WiFi scan completed successfully");

    scan_in_progress = false;

    // Copy results to our list
    for(int i = 0; i < found && i < MAX_AP_COUNT; i++) {
        strncpy(ap_list[ap_count].ssid, results[i].ssid, 32);
        ap_list[ap_count].ssid[32] = '\0';
        ap_list[ap_count].rssi = results[i].rssi;
        ap_count++;
    }

    // Create list items
    if(list_wifi) {
        for(int i = 0; i < ap_count; i++) {
            // Check if this AP is currently connected
            bool is_connected = (strcmp(ap_list[i].ssid, connected_ssid) == 0 && connected_ssid[0] != '\0');

            // Create button with appropriate color
            ap_list[i].btn = lv_list_add_btn(list_wifi, LV_SYMBOL_WIFI, ap_list[i].ssid);
            lv_obj_set_style_text_font(ap_list[i].btn, &lv_font_montserrat_14, 0);
            lv_obj_set_height(ap_list[i].btn, 55);
            lv_obj_add_flag(ap_list[i].btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(ap_list[i].btn, wifi_list_click_cb, LV_EVENT_CLICKED, NULL);

            // Set green color for connected network
            if(is_connected) {
                lv_obj_set_style_text_color(ap_list[i].btn, lv_palette_main(LV_PALETTE_GREEN), 0);
                ESP_LOGI(TAG, "Network %s is connected (shown in green)", ap_list[i].ssid);
            }
        }
    }

    // Update status
    if(label_status) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Found %d networks", ap_count);
        lv_label_set_text(label_status, buf);
        if(ap_count > 0) {
            lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_GREEN), 0);
        } else {
            lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_RED), 0);
        }
    }
}

// Keyboard slide-in animation callback (bottom to top) - LVGL 8.x compatible
static void keyboard_slide_in_anim_cb(lv_anim_t *a, int32_t value)
{
    // value: 0 -> 350
    // Animate mask position from y=320 to y=0
    int32_t start_y = 320;
    int32_t end_y = 0;
    int32_t current_y = start_y + (end_y - start_y) * value / 350;
    lv_obj_set_y(anim_mask, current_y);

    // Fade in opacity
    lv_obj_set_style_opa(anim_mask, LV_OPA_COVER * value / 350, 0);
}

// Back button color animation callback (blue to gray) - LVGL 8.x compatible
static void back_btn_color_anim_cb(lv_anim_t *a, int32_t value)
{
    // Color interpolation from blue (0x2195F6) to gray (0x9E9E9E)
    // Blue: 33, 149, 246
    // Gray: 158, 158, 158

    int32_t progress = value * 100 / 350;  // 0-100

    int32_t r = 33 + (158 - 33) * progress / 100;
    int32_t g = 149 + (158 - 149) * progress / 100;
    int32_t b = 246 + (158 - 246) * progress / 100;

    lv_obj_set_style_bg_color(anim_back_btn, lv_color_make(r, g, b), 0);
}

// WiFi back callback - 修复卡死问题
static void wifi_back_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        if(is_returning) return;
        is_returning = true;

        ESP_LOGI(TAG, "WiFi back button clicked");

        // Stop all timers first
        if(refresh_timer) {
            lv_timer_del(refresh_timer);
            refresh_timer = NULL;
        }

        if(connect_timer) {
            lv_timer_del(connect_timer);
            connect_timer = NULL;
        }

        if(scan_timer) {
            lv_timer_del(scan_timer);
            scan_timer = NULL;
        }

        // 清除消息框
        if(msgbox) {
            lv_msgbox_close(msgbox);
            msgbox = NULL;
        }

        // 删除键盘页面（如果存在）
        if(screen_keyboard) {
            lv_obj_del(screen_keyboard);
            screen_keyboard = NULL;
            keyboard = NULL;
            textarea_pwd = NULL;
        }

        // 先加载Settings页面（这样WiFi屏幕就不活跃了）
        lv_obj_t *settings_screen = ui_settings_get_screen();

        if(settings_screen == NULL) {
            // Settings页面不存在，创建它
            ui_settings_create();
            settings_screen = ui_settings_get_screen();
        }

        if(settings_screen) {
            lv_scr_load(settings_screen);
        }

        // 现在安全地删除WiFi屏幕
        if(screen_wifi) {
            // 先清空全局指针
            list_wifi = NULL;
            label_status = NULL;

            // 删除屏幕
            lv_obj_del(screen_wifi);
            screen_wifi = NULL;
        }

        // 重置状态
        is_connecting = false;
        scan_in_progress = false;
        ap_count = 0;
        wifi_initialized = false;
        is_returning = false;

        ESP_LOGI(TAG, "WiFi back completed");
    }
}

// Animation ready callback - load real keyboard screen after animation
static void keyboard_anim_ready_cb(lv_anim_t *a)
{
    // Delete mask
    if(anim_mask) {
        lv_obj_del(anim_mask);
        anim_mask = NULL;
    }
    anim_keyboard_content = NULL;
    anim_back_btn = NULL;

    // Create and load real keyboard screen
    if(screen_keyboard == NULL) {
        screen_keyboard = lv_obj_create(NULL);
        lv_obj_set_size(screen_keyboard, 240, 320);
        lv_obj_set_scrollbar_mode(screen_keyboard, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_color(screen_keyboard, lv_color_hex(0x0D1117), 0);
        lv_obj_set_style_bg_opa(screen_keyboard, LV_OPA_COVER, 0);

        // Back button (80x40, rounded square) - final gray color
        lv_obj_t *btn_back = lv_btn_create(screen_keyboard);
        lv_obj_set_size(btn_back, 80, 40);
        lv_obj_set_pos(btn_back, 5, 5);
        lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x9E9E9E), 0);  // Final gray color
        lv_obj_set_style_radius(btn_back, 18, 0);

        lv_obj_t *back_label = lv_label_create(btn_back);
        lv_label_set_text(back_label, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
        lv_obj_center(back_label);
        lv_obj_add_event_cb(btn_back, keyboard_back_cb, LV_EVENT_CLICKED, NULL);

        // SSID label
        lv_obj_t *label_ssid = lv_label_create(screen_keyboard);
        lv_label_set_text(label_ssid, selected_ssid);
        lv_obj_set_style_text_font(label_ssid, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label_ssid, lv_color_white(), 0);
        lv_obj_align(label_ssid, LV_ALIGN_TOP_MID, 0, 55);

        // Password input
        textarea_pwd = lv_textarea_create(screen_keyboard);
        lv_textarea_set_one_line(textarea_pwd, true);
        lv_textarea_set_password_mode(textarea_pwd, true);
        lv_textarea_set_placeholder_text(textarea_pwd, "Enter Password");
        lv_obj_set_size(textarea_pwd, 220, 45);
        lv_obj_align(textarea_pwd, LV_ALIGN_TOP_MID, 0, 80);
        lv_obj_set_style_bg_color(textarea_pwd, lv_color_hex(0x212121), 0);
        lv_obj_set_style_text_color(textarea_pwd, lv_color_white(), 0);
        lv_obj_set_style_radius(textarea_pwd, 18, 0);

        // Connect button
        lv_obj_t *btn_connect = lv_btn_create(screen_keyboard);
        lv_obj_set_size(btn_connect, 220, 45);
        lv_obj_align(btn_connect, LV_ALIGN_TOP_MID, 0, 135);
        lv_obj_set_style_bg_color(btn_connect, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_obj_set_style_radius(btn_connect, 18, 0);

        lv_obj_t *connect_label = lv_label_create(btn_connect);
        lv_label_set_text(connect_label, "Connect");
        lv_obj_set_style_text_color(connect_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(connect_label, &lv_font_montserrat_16, 0);
        lv_obj_center(connect_label);
        lv_obj_add_event_cb(btn_connect, keyboard_connect_cb, LV_EVENT_CLICKED, NULL);

        // Keyboard
        keyboard = lv_keyboard_create(screen_keyboard);
        lv_keyboard_set_textarea(keyboard, textarea_pwd);
        lv_obj_set_size(keyboard, 240, 135);
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);

        // Load keyboard screen
        lv_scr_load(screen_keyboard);
    }
}

// WiFi list click callback
static void wifi_list_click_cb(lv_event_t *e)
{
    if(is_connecting) return;

    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        lv_obj_t *btn = lv_event_get_target(e);

        // Find SSID
        for(int i = 0; i < ap_count; i++) {
            if(ap_list[i].btn == btn) {
                strncpy(selected_ssid, ap_list[i].ssid, 32);
                selected_ssid[32] = '\0';
                break;
            }
        }

        // Skip if "Scanning..."
        if(strcmp(selected_ssid, "Scanning...") == 0) {
            return;
        }

        // Check if this is the currently connected network
        if(strcmp(selected_ssid, connected_ssid) == 0 && app_main_is_wifi_connected()) {
            // Show disconnect confirmation dialog
            if(msgbox == NULL) {
                char msg[128];
                snprintf(msg, sizeof(msg), "Disconnect from\n%s?", selected_ssid);
                static const char *btns[] = {"Yes", "No", ""};
                msgbox = lv_msgbox_create(NULL, "Disconnect", msg, btns, false);
                lv_obj_add_event_cb(msgbox, disconnect_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
            }
            return;
        }

        // Create mask for slide-in animation
        if(anim_mask == NULL && screen_keyboard == NULL) {
            // Create mask on current screen
            anim_mask = lv_obj_create(lv_scr_act());
            lv_obj_set_size(anim_mask, 240, 320);
            lv_obj_set_style_bg_color(anim_mask, lv_color_hex(0x0D1117), 0);
            lv_obj_set_style_bg_opa(anim_mask, LV_OPA_COVER, 0);
            lv_obj_set_scrollbar_mode(anim_mask, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_style_border_width(anim_mask, 0, 0);
            lv_obj_set_style_pad_all(anim_mask, 0, 0);
            // Make mask non-clickable so it doesn't block events
            lv_obj_clear_flag(anim_mask, LV_OBJ_FLAG_CLICKABLE);

            // Set initial position (below screen, x=0 for center) and transparent
            lv_obj_set_pos(anim_mask, 0, 320);
            lv_obj_set_style_opa(anim_mask, LV_OPA_TRANSP, 0);

            // Create keyboard content on mask - position at (0,0)
            anim_keyboard_content = lv_obj_create(anim_mask);
            lv_obj_set_size(anim_keyboard_content, 240, 320);
            lv_obj_set_pos(anim_keyboard_content, 0, 0);
            lv_obj_set_style_bg_opa(anim_keyboard_content, LV_OPA_TRANSP, 0);
            lv_obj_set_scrollbar_mode(anim_keyboard_content, LV_SCROLLBAR_MODE_OFF);
            lv_obj_set_style_border_width(anim_keyboard_content, 0, 0);
            lv_obj_set_style_pad_all(anim_keyboard_content, 0, 0);

            // Back button - start with blue color
            lv_obj_t *btn_back = lv_btn_create(anim_keyboard_content);
            lv_obj_set_size(btn_back, 80, 40);
            lv_obj_set_pos(btn_back, 5, 5);
            lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);  // Start with blue
            lv_obj_set_style_radius(btn_back, 18, 0);
            anim_back_btn = btn_back;

            lv_obj_t *back_label = lv_label_create(btn_back);
            lv_label_set_text(back_label, LV_SYMBOL_LEFT);
            lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
            lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
            lv_obj_center(back_label);

            // SSID label
            lv_obj_t *label_ssid = lv_label_create(anim_keyboard_content);
            lv_label_set_text(label_ssid, selected_ssid);
            lv_obj_set_style_text_font(label_ssid, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(label_ssid, lv_color_white(), 0);
            lv_obj_align(label_ssid, LV_ALIGN_TOP_MID, 0, 55);

            // Password input placeholder
            lv_obj_t *textarea_pwd_dummy = lv_textarea_create(anim_keyboard_content);
            lv_textarea_set_one_line(textarea_pwd_dummy, true);
            lv_textarea_set_password_mode(textarea_pwd_dummy, true);
            lv_textarea_set_placeholder_text(textarea_pwd_dummy, "Enter Password");
            lv_obj_set_size(textarea_pwd_dummy, 220, 45);
            lv_obj_align(textarea_pwd_dummy, LV_ALIGN_TOP_MID, 0, 80);
            lv_obj_set_style_bg_color(textarea_pwd_dummy, lv_color_hex(0x212121), 0);
            lv_obj_set_style_text_color(textarea_pwd_dummy, lv_color_white(), 0);
            lv_obj_set_style_radius(textarea_pwd_dummy, 18, 0);

            // Connect button
            lv_obj_t *btn_connect = lv_btn_create(anim_keyboard_content);
            lv_obj_set_size(btn_connect, 220, 45);
            lv_obj_align(btn_connect, LV_ALIGN_TOP_MID, 0, 135);
            lv_obj_set_style_bg_color(btn_connect, lv_palette_main(LV_PALETTE_BLUE), 0);
            lv_obj_set_style_radius(btn_connect, 18, 0);

            lv_obj_t *connect_label = lv_label_create(btn_connect);
            lv_label_set_text(connect_label, "Connect");
            lv_obj_set_style_text_color(connect_label, lv_color_white(), 0);
            lv_obj_set_style_text_font(connect_label, &lv_font_montserrat_16, 0);
            lv_obj_center(connect_label);

            // Dummy keyboard
            lv_obj_t *keyboard_dummy = lv_keyboard_create(anim_keyboard_content);
            lv_obj_set_size(keyboard_dummy, 240, 135);
            lv_obj_align(keyboard_dummy, LV_ALIGN_BOTTOM_MID, 0, 0);

            // Move mask to foreground
            lv_obj_move_foreground(anim_mask);

            // Start slide-in animation (350ms)
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, anim_mask);
            lv_anim_set_time(&a, 350);
            lv_anim_set_delay(&a, 0);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)keyboard_slide_in_anim_cb);
            lv_anim_set_values(&a, 0, 350);
            lv_anim_set_ready_cb(&a, (lv_anim_ready_cb_t)keyboard_anim_ready_cb);
            lv_anim_start(&a);

            // Start back button color animation (350ms, 50ms delay)
            lv_anim_t b;
            lv_anim_init(&b);
            lv_anim_set_var(&b, anim_back_btn);
            lv_anim_set_time(&b, 350);
            lv_anim_set_delay(&b, 50);
            lv_anim_set_path_cb(&b, lv_anim_path_linear);
            lv_anim_set_exec_cb(&b, (lv_anim_exec_xcb_t)back_btn_color_anim_cb);
            lv_anim_set_values(&b, 0, 350);
            lv_anim_start(&b);
        }
    }
}

// Keyboard back callback - load WiFi first, then delete keyboard
static void keyboard_back_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        // Save keyboard screen pointer
        lv_obj_t *kb_screen = screen_keyboard;

        // Clear globals
        screen_keyboard = NULL;
        keyboard = NULL;
        textarea_pwd = NULL;

        // Load WiFi screen FIRST (this makes kb_screen inactive)
        if(screen_wifi != NULL) {
            lv_scr_load(screen_wifi);
        } else {
            ui_wifi_create();
            return;
        }

        // Then delete the old keyboard screen (now safe)
        if(kb_screen) {
            lv_obj_del(kb_screen);
        }
    }
}

// Connect callback
static void keyboard_connect_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED && textarea_pwd) {
        const char *pwd = lv_textarea_get_text(textarea_pwd);
        if(pwd && strlen(pwd) > 0) {
            is_connecting = true;

            if(label_status) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Connecting to %s...", selected_ssid);
                lv_label_set_text(label_status, buf);
                lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_BLUE), 0);
            }

            // Start WiFi connection
            ESP_LOGI(TAG, "Connecting to %s with password (len=%d)", selected_ssid, (int)strlen(pwd));

            // Configure WiFi (don't stop/start, WiFi is already running)
            wifi_config_t wifi_config = {0};

            // Copy SSID and password
            size_t ssid_len = strlen(selected_ssid);
            size_t pwd_len = strlen(pwd);

            ESP_LOGI(TAG, "SSID length: %d, Password length: %d", (int)ssid_len, (int)pwd_len);
            ESP_LOGI(TAG, "SSID: %s", selected_ssid);

            if(ssid_len > 32) ssid_len = 32;
            if(pwd_len > 63) pwd_len = 63;

            memcpy(wifi_config.sta.ssid, selected_ssid, ssid_len);
            memcpy(wifi_config.sta.password, pwd, pwd_len);

            // Set threshold to accept any auth mode (not just WPA2)
            wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

            esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
            if(ret != ESP_OK) {
                ESP_LOGE(TAG, "WiFi set config failed: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "WiFi config set successfully");
            }

            ret = esp_wifi_connect();
            if(ret != ESP_OK) {
                ESP_LOGE(TAG, "WiFi connect failed: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "WiFi connect command sent successfully");
            }

            ESP_LOGI(TAG, "WiFi connection started, waiting for events...");

            // Create connection timeout timer (20 seconds)
            if(connect_timer) lv_timer_del(connect_timer);
            connect_timer = lv_timer_create(connect_timeout_cb, 20000, NULL);
            lv_timer_set_repeat_count(connect_timer, 1);  // One-time timer

            // Save keyboard screen pointer
            lv_obj_t *kb_screen = screen_keyboard;

            // Clear globals
            screen_keyboard = NULL;
            keyboard = NULL;
            textarea_pwd = NULL;

            // Load WiFi screen FIRST
            if(screen_wifi != NULL) {
                lv_scr_load(screen_wifi);
            } else {
                ui_wifi_create();
                return;
            }

            // Then delete the old keyboard screen
            if(kb_screen) {
                lv_obj_del(kb_screen);
            }
        }
    }
}

// Connection timeout callback
static void connect_timeout_cb(lv_timer_t *t)
{
    ESP_LOGI(TAG, "Connection timeout callback triggered, is_connecting=%d", is_connecting);

    if(is_connecting) {
        bool actually_connected = app_main_is_wifi_connected();
        ESP_LOGI(TAG, "Actual WiFi connected status: %d", actually_connected);

        if(actually_connected) {
            // Connection successful!
            ESP_LOGI(TAG, "WiFi connection successful!");
            is_connecting = false;
            if(label_status) {
                char buf[64];
                snprintf(buf, sizeof(buf), "Connected to %s", selected_ssid);
                lv_label_set_text(label_status, buf);
                lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_GREEN), 0);
            }
        } else {
            // Connection failed
            ESP_LOGE(TAG, "WiFi connection failed after timeout");
            is_connecting = false;
            if(msgbox == NULL) {
                static const char *btns[] = {"OK", ""};

                // Get disconnect reason from main
                extern int get_last_disconnect_reason(void);
                int reason = get_last_disconnect_reason();

                char error_msg[128];
                if(reason == 201 || reason == 202) {
                    snprintf(error_msg, sizeof(error_msg),
                        "Connection failed!\nWrong password or\nAP not found.");
                } else if(reason == 14 || reason == 15) {
                    snprintf(error_msg, sizeof(error_msg),
                        "Connection failed!\nHandshake timeout.\nCheck password and\ntry WPA2/WPA mixed mode.");
                } else if(reason == 200) {
                    snprintf(error_msg, sizeof(error_msg),
                        "Connection failed!\nAP not found.\nMove closer and try again.");
                } else {
                    snprintf(error_msg, sizeof(error_msg),
                        "Connection timeout!\nReason code: %d\nCheck password and try again.", reason);
                }

                msgbox = lv_msgbox_create(NULL, "Error", error_msg, btns, true);
                lv_obj_add_event_cb(msgbox, msgbox_close_cb, LV_EVENT_ALL, NULL);
            }
            if(label_status) {
                lv_label_set_text(label_status, "Connection failed");
                lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_RED), 0);
            }
        }
    }
}

// Message box close callback
static void msgbox_close_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_CLICKED) {
        lv_obj_t *mbox = lv_event_get_current_target(e);
        if(mbox) {
            lv_msgbox_close(mbox);
        }
        msgbox = NULL;
    }
}

// Disconnect confirmation callback
static void disconnect_confirm_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_VALUE_CHANGED) {
        uint32_t btn_id = lv_msgbox_get_active_btn(lv_event_get_current_target(e));
        lv_obj_t *mbox = lv_event_get_current_target(e);

        if(btn_id == 0) {  // "Yes" button
            ESP_LOGI(TAG, "User confirmed disconnect");
            app_main_wifi_disconnect();
            connected_ssid[0] = '\0';  // Clear connected SSID
        } else {  // "No" button
            ESP_LOGI(TAG, "User cancelled disconnect");
        }

        if(mbox) {
            lv_msgbox_close(mbox);
        }
        msgbox = NULL;
    }
}

// Create WiFi page
void ui_wifi_create(void)
{
    // If already initialized, clean up old objects first
    if(wifi_initialized && screen_wifi != NULL) {
        // Stop all timers
        if(refresh_timer) {
            lv_timer_del(refresh_timer);
            refresh_timer = NULL;
        }
        if(connect_timer) {
            lv_timer_del(connect_timer);
            connect_timer = NULL;
        }
        if(scan_timer) {
            lv_timer_del(scan_timer);
            scan_timer = NULL;
        }
        // Delete old page
        lv_obj_del(screen_wifi);
        screen_wifi = NULL;
        list_wifi = NULL;
        label_status = NULL;
    }

    // Reset state
    is_connecting = false;
    is_returning = false;
    scan_in_progress = false;  // 确保重置扫描状态
    ap_count = 0;

    // Create WiFi page
    screen_wifi = lv_obj_create(NULL);
    lv_obj_set_size(screen_wifi, 240, 320);
    lv_obj_set_scrollbar_mode(screen_wifi, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_wifi, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_wifi, LV_OPA_COVER, 0);

    // Back button (80x40, rounded square)
    lv_obj_t *btn_back = lv_btn_create(screen_wifi);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, wifi_back_cb, LV_EVENT_CLICKED, NULL);

    // Title
    lv_obj_t *title = lv_label_create(screen_wifi);
    lv_label_set_text(title, "WiFi Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    // Refresh button (circular, top right) - same height as back button
    lv_obj_t *btn_refresh = lv_btn_create(screen_wifi);
    lv_obj_set_size(btn_refresh, 40, 40);  // Same height as back button (40)
    lv_obj_set_pos(btn_refresh, 195, 5);    // Adjusted position
    lv_obj_set_style_bg_color(btn_refresh, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(btn_refresh, 20, 0);  // Radius = half of size
    lv_obj_set_style_shadow_width(btn_refresh, 3, 0);
    lv_obj_set_style_shadow_opa(btn_refresh, LV_OPA_30, 0);
    lv_obj_set_style_shadow_ofs_y(btn_refresh, 2, 0);

    lv_obj_t *refresh_label = lv_label_create(btn_refresh);
    lv_label_set_text(refresh_label, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(refresh_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(refresh_label, &lv_font_montserrat_16, 0);
    lv_obj_center(refresh_label);
    lv_obj_add_event_cb(btn_refresh, refresh_btn_cb, LV_EVENT_CLICKED, NULL);

    // Status label
    label_status = lv_label_create(screen_wifi);
    lv_label_set_text(label_status, "Scanning...");
    lv_obj_set_style_text_font(label_status, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_align(label_status, LV_ALIGN_TOP_MID, 0, 70);

    // WiFi list
    list_wifi = lv_list_create(screen_wifi);
    lv_obj_set_size(list_wifi, 220, 200);
    lv_obj_set_pos(list_wifi, 10, 95);
    lv_obj_set_style_bg_color(list_wifi, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(list_wifi, 12, 0);
    lv_obj_set_style_border_width(list_wifi, 0, 0);
    lv_obj_set_style_pad_all(list_wifi, 8, 0);

    // Enable scrolling and click flags
    lv_obj_set_scrollbar_mode(list_wifi, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(list_wifi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list_wifi, LV_OBJ_FLAG_CLICKABLE);
    // Don't use snap - it causes jumping to top
    // lv_obj_set_scroll_snap_y(list_wifi, LV_SCROLL_SNAP_START);

    lv_scr_load(screen_wifi);

    // 初始化WiFi scanner（只初始化一次）
    static bool scanner_initialized = false;
    if(!scanner_initialized) {
        ESP_LOGI(TAG, "Initializing WiFi scanner...");
        if(wifi_scanner_init() == ESP_OK) {
            ESP_LOGI(TAG, "WiFi scanner initialized successfully");
            scanner_initialized = true;
        } else {
            ESP_LOGE(TAG, "WiFi scanner initialization failed");
        }
    }

    // 删除旧的定时器（重要！）
    if(scan_timer) {
        lv_timer_del(scan_timer);
        scan_timer = NULL;
    }

    // 根据WiFi状态决定是否触发连接
    bool is_connected = app_main_is_wifi_connected();

    if(!is_connected) {
        // WiFi未连接：尝试连接已保存的WiFi（只尝试1次）
        ESP_LOGI(TAG, "WiFi not connected, triggering auto-connect");
        app_main_trigger_wifi_connect();
    } else {
        ESP_LOGI(TAG, "WiFi already connected, skipping trigger");
    }

    // 设置状态
    if(label_status) {
        lv_label_set_text(label_status, "Scanning...");
        lv_obj_set_style_text_color(label_status, lv_palette_main(LV_PALETTE_BLUE), 0);
    }

    // 延迟扫描，让UI先渲染完成
    scan_timer = lv_timer_create(do_wifi_scan_timer, 100, NULL);
    lv_timer_set_repeat_count(scan_timer, 1);

    wifi_initialized = true;
}

// Get WiFi page screen
lv_obj_t* ui_wifi_get_screen(void)
{
    return screen_wifi;
}
