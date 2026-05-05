/**
 * History page.
 *
 * Shows saved model name and creation time records.
 */

#include "ui_history.h"
#include "ui_home.h"
#include "../history_store.h"
#include "../developer_mode.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_history";
static lv_obj_t *screen_history = NULL;
static history_record_t history_records[HISTORY_STORE_MAX_RECORDS];
static lv_obj_t *rename_screen = NULL;
static lv_obj_t *rename_textarea = NULL;
static int rename_storage_index = -1;
static char pending_rename_name[64];
static int pending_rename_storage_index = -1;
static history_record_t history_test_records[10];
static lv_obj_t *history_test_name_labels[10];
static bool history_test_records_ready = false;

static void refresh_history_page(void);

void ui_history_destroy(void)
{
    lv_obj_t *old_rename = rename_screen;
    lv_obj_t *old_history = screen_history;

    rename_screen = NULL;
    rename_textarea = NULL;
    rename_storage_index = -1;
    pending_rename_storage_index = -1;
    pending_rename_name[0] = '\0';
    screen_history = NULL;

    for (int i = 0; i < 10; i++) {
        history_test_name_labels[i] = NULL;
    }

    if (old_rename) {
        lv_obj_del_async(old_rename);
    }
    if (old_history) {
        lv_obj_del_async(old_history);
    }
}

static void apply_light_style(lv_obj_t *obj)
{
    if (!obj) {
        return;
    }
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
}

static void init_history_test_records(void)
{
    if (history_test_records_ready) {
        return;
    }

    for (int i = 0; i < 10; i++) {
        snprintf(history_test_records[i].model_name, sizeof(history_test_records[i].model_name), "Test Model %02d", i + 1);
        snprintf(history_test_records[i].created_time, sizeof(history_test_records[i].created_time), "2026-05-05 %02d:%02d", 9 + i, 10 + i);
        snprintf(history_test_records[i].model_size, sizeof(history_test_records[i].model_size), "%d.%d MB", 2 + i, i % 10);
        snprintf(history_test_records[i].pointcloud_size, sizeof(history_test_records[i].pointcloud_size), "%d.%d MB", 1 + i, (i + 4) % 10);
        snprintf(history_test_records[i].model_path, sizeof(history_test_records[i].model_path), "/data/test_picture/model/test_%02d", i + 1);
        history_test_records[i].storage_index = -1 - i;
    }

    history_test_records_ready = true;
}

static void back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_scr_load(ui_home_get_screen());
        ui_history_destroy();
    }
}

static void rename_back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && screen_history && rename_screen) {
        lv_obj_t *old_rename = rename_screen;
        rename_screen = NULL;
        rename_textarea = NULL;
        rename_storage_index = -1;
        lv_scr_load(screen_history);
        lv_obj_del_async(old_rename);
    }
}

static void refresh_history_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    refresh_history_page();
}

static void close_rename_screen_only(void)
{
    lv_obj_t *old_rename = rename_screen;
    rename_screen = NULL;
    rename_textarea = NULL;
    rename_storage_index = -1;

    if (screen_history) {
        lv_scr_load(screen_history);
    }

    if (old_rename) {
        lv_obj_del_async(old_rename);
    }
}

static void rename_save_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    bool need_full_refresh = false;

    if (pending_rename_name[0] != '\0') {
        if (pending_rename_storage_index >= 0) {
            esp_err_t err = history_store_rename(pending_rename_storage_index, pending_rename_name);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "rename saved: %s", pending_rename_name);
                need_full_refresh = true;
            } else {
                ESP_LOGE(TAG, "rename failed: %s", esp_err_to_name(err));
            }
        } else {
            int test_index = -1 - pending_rename_storage_index;
            if (test_index >= 0 && test_index < 10) {
                snprintf(history_test_records[test_index].model_name,
                         sizeof(history_test_records[test_index].model_name), "%s", pending_rename_name);
                ESP_LOGI(TAG, "test rename saved: %s", pending_rename_name);
                if (history_test_name_labels[test_index]) {
                    lv_label_set_text(history_test_name_labels[test_index], pending_rename_name);
                }
            }
        }
    }

    pending_rename_storage_index = -1;
    pending_rename_name[0] = '\0';

    close_rename_screen_only();

    if (need_full_refresh) {
        lv_timer_t *refresh_timer = lv_timer_create(refresh_history_timer_cb, 120, NULL);
        (void)refresh_timer;
    }
}

static void rename_save_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !rename_textarea) {
        return;
    }

    lv_obj_t *save_btn = lv_event_get_target(e);
    if (save_btn) {
        lv_obj_add_state(save_btn, LV_STATE_DISABLED);
    }

    const char *new_name = lv_textarea_get_text(rename_textarea);
    snprintf(pending_rename_name, sizeof(pending_rename_name), "%s", new_name ? new_name : "");
    pending_rename_storage_index = rename_storage_index;

    lv_timer_t *timer = lv_timer_create(rename_save_timer_cb, 1, NULL);
    (void)timer;
}

static void delete_existing_rename_screen(void)
{
    if (rename_screen) {
        lv_obj_del_async(rename_screen);
        rename_screen = NULL;
        rename_textarea = NULL;
    }
    rename_storage_index = -1;
}

static void open_rename_keyboard(const history_record_t *record)
{
    if (!record) {
        return;
    }

    delete_existing_rename_screen();

    rename_storage_index = record->storage_index;
    rename_screen = lv_obj_create(NULL);
    lv_obj_set_size(rename_screen, 240, 320);
    lv_obj_set_scrollbar_mode(rename_screen, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(rename_screen, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(rename_screen, LV_OPA_COVER, 0);
    apply_light_style(rename_screen);

    lv_obj_t *btn_back = lv_btn_create(rename_screen);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x9E9E9E), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);
    apply_light_style(btn_back);
    lv_obj_add_event_cb(btn_back, rename_back_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(rename_screen);
    lv_label_set_text(title, "Rename Model");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 55);

    rename_textarea = lv_textarea_create(rename_screen);
    lv_textarea_set_one_line(rename_textarea, true);
    lv_textarea_set_text(rename_textarea, record->model_name);
    lv_textarea_set_placeholder_text(rename_textarea, "Model Name");
    lv_obj_set_size(rename_textarea, 220, 45);
    lv_obj_align(rename_textarea, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(rename_textarea, lv_color_hex(0x212121), 0);
    lv_obj_set_style_text_color(rename_textarea, lv_color_white(), 0);
    lv_obj_set_style_radius(rename_textarea, 18, 0);
    apply_light_style(rename_textarea);

    lv_obj_t *btn_save = lv_btn_create(rename_screen);
    lv_obj_set_size(btn_save, 220, 45);
    lv_obj_align(btn_save, LV_ALIGN_TOP_MID, 0, 135);
    lv_obj_set_style_bg_color(btn_save, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_radius(btn_save, 18, 0);
    apply_light_style(btn_save);
    lv_obj_add_event_cb(btn_save, rename_save_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *save_label = lv_label_create(btn_save);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(save_label, &lv_font_montserrat_16, 0);
    lv_obj_center(save_label);

    lv_obj_t *keyboard = lv_keyboard_create(rename_screen);
    lv_keyboard_set_textarea(keyboard, rename_textarea);
    lv_obj_set_size(keyboard, 240, 135);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    apply_light_style(keyboard);

    lv_scr_load(rename_screen);
}

static void history_row_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        history_record_t *record = (history_record_t *)lv_event_get_user_data(e);
        open_rename_keyboard(record);
    }
}

static void add_history_row(lv_obj_t *parent, history_record_t *record)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, 208);
    lv_obj_set_height(row, 82);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_shadow_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, history_row_callback, LV_EVENT_LONG_PRESSED, record);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, record->model_name);
    lv_obj_set_width(name, 188);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0x1F2937), 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

    if (record->storage_index < 0) {
        int test_index = -1 - record->storage_index;
        if (test_index >= 0 && test_index < 10) {
            history_test_name_labels[test_index] = name;
        }
    }

    lv_obj_t *time = lv_label_create(row);
    lv_label_set_text(time, record->created_time);
    lv_obj_set_width(time, 188);
    lv_label_set_long_mode(time, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time, lv_color_hex(0x6B7280), 0);
    lv_obj_align(time, LV_ALIGN_TOP_LEFT, 0, 22);

    char size_text[80];
    snprintf(size_text, sizeof(size_text), "Model: %s  Point: %s", record->model_size, record->pointcloud_size);
    lv_obj_t *sizes = lv_label_create(row);
    lv_label_set_text(sizes, size_text);
    lv_obj_set_width(sizes, 188);
    lv_label_set_long_mode(sizes, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(sizes, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(sizes, lv_color_hex(0x4B5563), 0);
    lv_obj_align(sizes, LV_ALIGN_TOP_LEFT, 0, 44);

    lv_obj_t *path = lv_label_create(row);
    lv_label_set_text(path, record->model_path);
    lv_obj_set_width(path, 188);
    lv_label_set_long_mode(path, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(path, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(path, lv_color_hex(0x9CA3AF), 0);
    lv_obj_align(path, LV_ALIGN_TOP_LEFT, 0, 62);
}

static lv_obj_t *pending_load_list = NULL;

static void load_history_records_cb(lv_timer_t *timer)
{
    lv_obj_t *list = pending_load_list;
    pending_load_list = NULL;

    history_store_init();

    size_t count = 0;
    if (developer_mode_is_history_test()) {
        init_history_test_records();
        count = 10;
        for (size_t i = 0; i < count; i++) {
            history_records[i] = history_test_records[i];
        }
    } else {
        count = history_store_get_all(history_records, HISTORY_STORE_MAX_RECORDS);
    }
    ESP_LOGI(TAG, "loaded %d history records", (int)count);

    if (count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, "No model history");
        lv_obj_set_width(empty, 224);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(0x6B7280), 0);
    } else {
        for (size_t i = 0; i < count; i++) {
            add_history_row(list, &history_records[i]);
        }
    }
}

static void refresh_history_page(void)
{
    for (int i = 0; i < 10; i++) {
        history_test_name_labels[i] = NULL;
    }

    ui_history_destroy();

    screen_history = lv_obj_create(NULL);
    lv_obj_set_size(screen_history, 240, 320);
    lv_obj_set_scrollbar_mode(screen_history, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_history, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_history, LV_OPA_COVER, 0);

    lv_obj_t *back_btn = lv_btn_create(screen_history);
    lv_obj_set_size(back_btn, 80, 40);
    lv_obj_set_pos(back_btn, 5, 5);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_radius(back_btn, 18, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, back_callback, LV_EVENT_ALL, NULL);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);

    lv_obj_t *title = lv_label_create(screen_history);
    lv_label_set_text(title, "History");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x1A237E), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *list = lv_obj_create(screen_history);
    lv_obj_set_size(list, 224, 252);
    lv_obj_set_pos(list, 8, 58);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    lv_scr_load(screen_history);

    pending_load_list = list;
    lv_timer_t *load_timer = lv_timer_create(load_history_records_cb, 50, NULL);
    lv_timer_set_repeat_count(load_timer, 1);
}

void ui_history_create(void)
{
    refresh_history_page();
}

lv_obj_t *ui_history_get_screen(void)
{
    return screen_history;
}
