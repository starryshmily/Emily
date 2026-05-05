#include "ui_developer.h"
#include "ui_settings.h"
#include "../developer_mode.h"
#include "../c3_uart.h"
#include "esp_log.h"

static const char *TAG = "ui_developer";
static lv_obj_t *screen_developer = NULL;

static void back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_scr_load(ui_settings_get_screen());
    }
}

static void upload_test_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        developer_mode_set_upload_test(enabled);
        ESP_LOGI(TAG, "Upload Test: %s", enabled ? "ON" : "OFF");
    }
}

static void history_test_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        developer_mode_set_history_test(enabled);
        ESP_LOGI(TAG, "History Test: %s", enabled ? "ON" : "OFF");
    }
}

static void uart_test_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        developer_mode_set_uart_test(enabled);
        c3_uart_send(enabled ? "BEEP:ON" : "BEEP:OFF");
        ESP_LOGI(TAG, "UART Test: %s", enabled ? "ON" : "OFF");
    }
}

static void add_switch_row(lv_obj_t *parent, const char *text, bool checked, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 208, 52);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x1F2937), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (checked) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void ui_developer_create(void)
{
    developer_mode_init();

    if (screen_developer) {
        lv_obj_del(screen_developer);
        screen_developer = NULL;
    }

    screen_developer = lv_obj_create(NULL);
    lv_obj_set_size(screen_developer, 240, 320);
    lv_obj_set_scrollbar_mode(screen_developer, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_developer, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_developer, LV_OPA_COVER, 0);

    lv_obj_t *btn_back = lv_btn_create(screen_developer);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, back_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(screen_developer);
    lv_label_set_text(title, "Developer Mode");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    lv_obj_t *list = lv_obj_create(screen_developer);
    lv_obj_set_size(list, 224, 210);
    lv_obj_set_pos(list, 8, 85);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    add_switch_row(list, "Upload Test", developer_mode_is_upload_test(), upload_test_cb);
    add_switch_row(list, "History Test", developer_mode_is_history_test(), history_test_cb);
    add_switch_row(list, "UART Test", developer_mode_is_uart_test(), uart_test_cb);

    lv_scr_load(screen_developer);
}

lv_obj_t *ui_developer_get_screen(void)
{
    return screen_developer;
}
