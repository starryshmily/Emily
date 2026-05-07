#include "ui_developer.h"
#include "ui_settings.h"
#include "../developer_mode.h"
#include "../c3_uart.h"
#include "../log_store.h"
#include "esp_log.h"
#include <stdint.h>
#include <string.h>

static const char *TAG = "ui_developer";
static lv_obj_t *screen_developer = NULL;
static lv_obj_t *screen_log = NULL;
static lv_obj_t *log_box = NULL;
static lv_timer_t *log_refresh_timer = NULL;
static uint32_t log_scroll_until_ms = 0;

#define LOG_PAGE_SNAPSHOT_SIZE 2200
#define LOG_PAGE_MAX_LINES 24
#define LOG_PAGE_LINE_LEN 96

static char log_snapshot[LOG_PAGE_SNAPSHOT_SIZE];
static uint32_t log_snapshot_hash = 0;

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

static void log_test_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        bool enabled = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
        developer_mode_set_log_test(enabled);
        if (!enabled) {
            log_store_clear();
        }
        ESP_LOGI(TAG, "Log Record: %s", enabled ? "ON" : "OFF");
    }
}

static void log_page_destroy(void)
{
    if (log_refresh_timer) {
        lv_timer_del(log_refresh_timer);
        log_refresh_timer = NULL;
    }
    log_box = NULL;
    log_scroll_until_ms = 0;
    log_snapshot_hash = 0;
    if (screen_log) {
        lv_obj_t *old = screen_log;
        screen_log = NULL;
        lv_obj_del_async(old);
    }
}

static void log_back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (screen_developer) {
        lv_scr_load(screen_developer);
    } else {
        ui_developer_create();
    }
    log_page_destroy();
}

static void log_clear_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    log_store_clear();
    if (screen_developer) {
        lv_scr_load(screen_developer);
    } else {
        ui_developer_create();
    }
    log_page_destroy();
}

static void log_refresh_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!log_box) return;
    if (log_scroll_until_ms != 0 && (int32_t)(lv_tick_get() - log_scroll_until_ms) < 0) return;

    size_t len = log_store_snapshot(log_snapshot, sizeof(log_snapshot));
    uint32_t hash = 2166136261u;
    const char *text = (len == 0) ? "No log" : log_snapshot;
    for (const char *p = text; *p; p++) {
        hash ^= (uint8_t)*p;
        hash *= 16777619u;
    }
    if (hash == log_snapshot_hash) return;
    log_snapshot_hash = hash;

    lv_obj_clean(log_box);

    char lines[LOG_PAGE_MAX_LINES][LOG_PAGE_LINE_LEN];
    int line_count = 0;
    char *line_start = (char *)text;
    char *p = (char *)text;
    while (1) {
        if (*p == '\n' || *p == '\0') {
            size_t line_len = (size_t)(p - line_start);
            while (line_len > 0 && (line_start[line_len - 1] == '\r' || line_start[line_len - 1] == '\n')) {
                line_len--;
            }
            if (line_count == LOG_PAGE_MAX_LINES) {
                memmove(lines, lines + 1, (LOG_PAGE_MAX_LINES - 1) * LOG_PAGE_LINE_LEN);
                line_count = LOG_PAGE_MAX_LINES - 1;
            }
            if (line_len >= LOG_PAGE_LINE_LEN) line_len = LOG_PAGE_LINE_LEN - 1;
            memcpy(lines[line_count], line_start, line_len);
            lines[line_count][line_len] = '\0';
            line_count++;
            if (*p == '\0') break;
            line_start = p + 1;
        }
        p++;
    }

    for (int i = 0; i < line_count; i++) {
        if (lines[i][0] == '\0') continue;
        lv_obj_t *line = lv_label_create(log_box);
        lv_obj_set_width(line, 204);
        lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);
        lv_label_set_text(line, lines[i]);
        lv_obj_set_style_text_font(line, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(line, lv_color_hex(0x111827), 0);
    }
}

static void log_box_scroll_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_SCROLL) return;
    log_scroll_until_ms = lv_tick_get() + 2000;
}

static void show_log_page(void)
{
    log_page_destroy();

    screen_log = lv_obj_create(NULL);
    lv_obj_set_size(screen_log, 240, 320);
    lv_obj_set_scrollbar_mode(screen_log, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_log, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_log, LV_OPA_COVER, 0);

    lv_obj_t *btn_back = lv_btn_create(screen_log);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    lv_obj_add_event_cb(btn_back, log_back_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);

    lv_obj_t *btn_delete = lv_btn_create(screen_log);
    lv_obj_set_size(btn_delete, 70, 35);
    lv_obj_set_pos(btn_delete, 162, 7);
    lv_obj_set_style_bg_color(btn_delete, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_bg_opa(btn_delete, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_delete, 8, 0);
    lv_obj_set_style_border_width(btn_delete, 0, 0);
    lv_obj_set_style_shadow_width(btn_delete, 0, 0);
    lv_obj_add_event_cb(btn_delete, log_clear_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *del_label = lv_label_create(btn_delete);
    lv_label_set_text(del_label, "Delete");
    lv_obj_set_style_text_color(del_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(del_label, &lv_font_montserrat_12, 0);
    lv_obj_center(del_label);

    lv_obj_t *title = lv_label_create(screen_log);
    lv_label_set_text(title, "Log");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 45);

    log_box = lv_obj_create(screen_log);
    lv_obj_set_size(log_box, 224, 230);
    lv_obj_set_pos(log_box, 8, 82);
    lv_obj_set_style_bg_color(log_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(log_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(log_box, 8, 0);
    lv_obj_set_style_border_width(log_box, 0, 0);
    lv_obj_set_style_shadow_width(log_box, 0, 0);
    lv_obj_set_style_pad_all(log_box, 8, 0);
    lv_obj_set_style_pad_row(log_box, 6, 0);
    lv_obj_set_flex_flow(log_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(log_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_event_cb(log_box, log_box_scroll_cb, LV_EVENT_SCROLL, NULL);

    log_refresh_timer_cb(NULL);
    log_refresh_timer = lv_timer_create(log_refresh_timer_cb, 2000, NULL);

    lv_scr_load(screen_log);
}

static void log_row_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    show_log_page();
}

static void add_action_row(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 208, 52);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x1F2937), 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *arrow = lv_label_create(row);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x6B7280), 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
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
    log_page_destroy();

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
    lv_obj_set_size(list, 224, 220);
    lv_obj_set_pos(list, 8, 85);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    add_switch_row(list, "Upload Test", developer_mode_is_upload_test(), upload_test_cb);
    add_switch_row(list, "History Test", developer_mode_is_history_test(), history_test_cb);
    add_switch_row(list, "UART Test", developer_mode_is_uart_test(), uart_test_cb);
    add_switch_row(list, "Log Record", developer_mode_is_log_test(), log_test_cb);
    add_action_row(list, "Log", log_row_cb);

    lv_scr_load(screen_developer);
}

lv_obj_t *ui_developer_get_screen(void)
{
    return screen_developer;
}
