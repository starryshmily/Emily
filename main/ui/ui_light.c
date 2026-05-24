#include "lvgl.h"
#include "ui_light.h"
#include "ui_settings.h"
#include "settings_store.h"
#include "c3_uart.h"
#include <stdio.h>

static lv_obj_t *screen_light = NULL;
static lv_obj_t *screen_color = NULL;
static lv_obj_t *color_slider = NULL;
static lv_obj_t *color_value_label = NULL;
static lv_obj_t *color_preview = NULL;

static void hue_to_rgb(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = hue / 43;
    uint8_t remainder = (hue - (region * 43)) * 6;
    uint8_t p = 0;
    uint8_t q = 255 - remainder;
    uint8_t t = remainder;

    switch (region) {
    case 0: *r = 255; *g = t;   *b = p;   break;
    case 1: *r = q;   *g = 255; *b = p;   break;
    case 2: *r = p;   *g = 255; *b = t;   break;
    case 3: *r = p;   *g = q;   *b = 255; break;
    case 4: *r = t;   *g = p;   *b = 255; break;
    default:*r = 255; *g = p;   *b = q;   break;
    }
}

static void send_home_color(void)
{
    if (!settings_store_is_ws2812_enabled()) {
        c3_uart_send("WS2812:OFF");
        return;
    }

    uint8_t r, g, b;
    hue_to_rgb(settings_store_get_light_color(), &r, &g, &b);
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "WS2812:COLOR:%u:%u:%u", r, g, b);
    c3_uart_send(cmd);
}

static void light_back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_scr_load(ui_settings_get_screen());
    }
}

static void color_back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        if (screen_light) {
            lv_scr_load(screen_light);
        } else {
            ui_light_create();
        }
    }
}

static void light_switch_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        settings_store_set_ws2812_enabled(enabled);
        send_home_color();
    }
}

static void color_row_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_light_color_create();
    }
}

static void update_color_ui(uint8_t hue)
{
    uint8_t r, g, b;
    hue_to_rgb(hue, &r, &g, &b);
    lv_color_t color = lv_color_make(r, g, b);

    if (color_preview) {
        lv_obj_set_style_bg_color(color_preview, color, 0);
    }
    if (color_slider) {
        lv_obj_set_style_bg_color(color_slider, color, LV_PART_KNOB);
        lv_obj_set_style_bg_color(color_slider, lv_color_lighten(color, 90), LV_PART_INDICATOR);
    }
    if (color_value_label) {
        char buf[20];
        snprintf(buf, sizeof(buf), "%d", hue);
        lv_label_set_text(color_value_label, buf);
        lv_obj_set_style_text_color(color_value_label, color, 0);
    }
}

static void color_slider_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        uint8_t hue = (uint8_t)lv_slider_get_value(color_slider);
        settings_store_set_light_color(hue);
        update_color_ui(hue);
        send_home_color();
    }
}

void ui_light_create(void)
{
    settings_store_init();
    if (screen_light) {
        lv_obj_del(screen_light);
        screen_light = NULL;
    }

    screen_light = lv_obj_create(NULL);
    lv_obj_set_size(screen_light, 240, 320);
    lv_obj_set_scrollbar_mode(screen_light, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_light, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_light, LV_OPA_COVER, 0);

    lv_obj_t *btn_back = lv_btn_create(screen_light);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, light_back_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(screen_light);
    lv_label_set_text(title, "Light");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    lv_obj_t *list = lv_obj_create(screen_light);
    lv_obj_set_size(list, 224, 170);
    lv_obj_set_pos(list, 8, 95);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *row_on = lv_obj_create(list);
    lv_obj_set_size(row_on, 208, 52);
    lv_obj_set_style_bg_color(row_on, lv_color_white(), 0);
    lv_obj_set_style_radius(row_on, 8, 0);
    lv_obj_set_style_border_width(row_on, 0, 0);
    lv_obj_set_style_pad_all(row_on, 8, 0);
    lv_obj_clear_flag(row_on, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *on_label = lv_label_create(row_on);
    lv_label_set_text(on_label, "ON/OFF");
    lv_obj_set_style_text_font(on_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(on_label, lv_color_hex(0x1F2937), 0);
    lv_obj_align(on_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row_on);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (settings_store_is_ws2812_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, light_switch_callback, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *row_color = lv_obj_create(list);
    lv_obj_set_size(row_color, 208, 52);
    lv_obj_set_style_bg_color(row_color, lv_color_white(), 0);
    lv_obj_set_style_radius(row_color, 8, 0);
    lv_obj_set_style_border_width(row_color, 0, 0);
    lv_obj_set_style_pad_all(row_color, 8, 0);
    lv_obj_clear_flag(row_color, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row_color, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row_color, color_row_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *color_label = lv_label_create(row_color);
    lv_label_set_text(color_label, "Color");
    lv_obj_set_style_text_font(color_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(color_label, lv_color_hex(0x1F2937), 0);
    lv_obj_align(color_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *arrow = lv_label_create(row_color);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x6B7280), 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_scr_load(screen_light);
}

void ui_light_color_create(void)
{
    settings_store_init();
    if (screen_color) {
        lv_obj_del(screen_color);
        screen_color = NULL;
    }

    screen_color = lv_obj_create(NULL);
    lv_obj_set_size(screen_color, 240, 320);
    lv_obj_set_scrollbar_mode(screen_color, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_color, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(screen_color, LV_OPA_COVER, 0);

    lv_obj_t *btn_back = lv_btn_create(screen_color);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, color_back_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(screen_color);
    lv_label_set_text(title, "Color");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    color_preview = lv_obj_create(screen_color);
    lv_obj_set_size(color_preview, 64, 40);
    lv_obj_align(color_preview, LV_ALIGN_CENTER, 0, -70);
    lv_obj_set_style_radius(color_preview, 8, 0);
    lv_obj_set_style_border_width(color_preview, 0, 0);

    color_slider = lv_slider_create(screen_color);
    lv_obj_set_size(color_slider, 200, 15);
    lv_obj_align(color_slider, LV_ALIGN_CENTER, 0, -10);
    lv_slider_set_range(color_slider, 0, 255);
    lv_slider_set_value(color_slider, settings_store_get_light_color(), LV_ANIM_OFF);
    lv_obj_add_event_cb(color_slider, color_slider_callback, LV_EVENT_VALUE_CHANGED, NULL);

    color_value_label = lv_label_create(screen_color);
    lv_obj_set_style_text_font(color_value_label, &lv_font_montserrat_24, 0);
    lv_obj_align(color_value_label, LV_ALIGN_CENTER, 0, 50);
    update_color_ui(settings_store_get_light_color());

    lv_scr_load(screen_color);
}

lv_obj_t *ui_light_get_screen(void)
{
    return screen_light;
}
