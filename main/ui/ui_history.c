/**
 * History page.
 *
 * Lightweight list (name+time) → detail page (full info + delete).
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
static history_record_t history_test_records[10];
static bool history_test_records_ready = false;

// Detail page
static lv_obj_t *screen_detail = NULL;
static history_record_t detail_record;

// Rename keyboard
static lv_obj_t *rename_screen = NULL;
static lv_obj_t *rename_textarea = NULL;
static char pending_rename_name[64];
static int pending_rename_storage_index = -1;

static void refresh_history_page(void);

void ui_history_destroy(void)
{
    lv_obj_t *old_history = screen_history;
    lv_obj_t *old_detail = screen_detail;
    lv_obj_t *old_rename = rename_screen;
    screen_history = NULL;
    screen_detail = NULL;
    rename_screen = NULL;
    rename_textarea = NULL;
    pending_rename_storage_index = -1;
    pending_rename_name[0] = '\0';

    if (old_rename) {
        lv_obj_del_async(old_rename);
    }
    if (old_detail) {
        lv_obj_del_async(old_detail);
    }
    if (old_history) {
        lv_obj_del_async(old_history);
    }
}

static void apply_light_style(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
}

static void init_history_test_records(void)
{
    if (history_test_records_ready) return;

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

// ============== Back to home ==============

static void back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        lv_scr_load(ui_home_get_screen());
        ui_history_destroy();
    }
}

// ============== Detail page ==============

static void detail_back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!screen_history || !screen_detail) return;

    lv_obj_t *old = screen_detail;
    screen_detail = NULL;
    lv_scr_load(screen_history);
    lv_obj_del_async(old);
}

static void refresh_after_delete_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    refresh_history_page();
}

static void detail_delete_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (detail_record.storage_index >= 0) {
        esp_err_t err = history_store_delete(detail_record.storage_index);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Deleted record: %s", detail_record.model_name);
        } else {
            ESP_LOGE(TAG, "Delete failed: %s", esp_err_to_name(err));
        }
    } else {
        // Test mode: mark the test record as deleted
        int test_index = -1 - detail_record.storage_index;
        if (test_index >= 0 && test_index < 10) {
            history_test_records[test_index].model_name[0] = '\0';
            ESP_LOGI(TAG, "Test record %d marked deleted", test_index);
        }
    }

    // Load history screen back first (LVGL needs a valid active screen)
    if (screen_history) {
        lv_scr_load(screen_history);
    }

    // Destroy detail screen
    lv_obj_t *old = screen_detail;
    screen_detail = NULL;
    if (old) lv_obj_del_async(old);

    // Refresh history list with delay
    lv_timer_t *t = lv_timer_create(refresh_after_delete_timer_cb, 100, NULL);
    (void)t;
}

// ============== Rename from detail page ==============

static void rename_back_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *old = rename_screen;
    rename_screen = NULL;
    rename_textarea = NULL;
    if (screen_detail) {
        lv_scr_load(screen_detail);
    }
    if (old) lv_obj_del_async(old);
}

static void refresh_after_rename_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    refresh_history_page();
}

static void rename_save_timer_cb(lv_timer_t *timer)
{
    lv_timer_del(timer);
    bool need_refresh = false;

    if (pending_rename_name[0] != '\0') {
        if (pending_rename_storage_index >= 0) {
            esp_err_t err = history_store_rename(pending_rename_storage_index, pending_rename_name);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "Rename saved: %s", pending_rename_name);
                need_refresh = true;
            } else {
                ESP_LOGE(TAG, "Rename failed: %s", esp_err_to_name(err));
            }
        } else {
            // Test mode: rename the test record
            int test_index = -1 - pending_rename_storage_index;
            if (test_index >= 0 && test_index < 10) {
                snprintf(history_test_records[test_index].model_name,
                         sizeof(history_test_records[test_index].model_name), "%s", pending_rename_name);
                ESP_LOGI(TAG, "Test rename saved: %s", pending_rename_name);
                need_refresh = true;
            }
        }
    }

    pending_rename_storage_index = -1;
    pending_rename_name[0] = '\0';

    // Load history screen first (LVGL needs valid active screen)
    if (need_refresh && screen_history) {
        lv_scr_load(screen_history);
    }

    // Close rename screen
    lv_obj_t *old = rename_screen;
    rename_screen = NULL;
    rename_textarea = NULL;
    if (old) lv_obj_del_async(old);

    if (need_refresh) {
        // Also close detail and refresh list
        lv_obj_t *old_detail = screen_detail;
        screen_detail = NULL;
        if (old_detail) lv_obj_del_async(old_detail);
        lv_timer_t *t = lv_timer_create(refresh_after_rename_timer_cb, 100, NULL);
        (void)t;
    } else if (screen_detail) {
        lv_scr_load(screen_detail);
    }
}

static void rename_save_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || !rename_textarea) return;

    lv_obj_t *save_btn = lv_event_get_target(e);
    if (save_btn) lv_obj_add_state(save_btn, LV_STATE_DISABLED);

    const char *new_name = lv_textarea_get_text(rename_textarea);
    snprintf(pending_rename_name, sizeof(pending_rename_name), "%s", new_name ? new_name : "");
    pending_rename_storage_index = detail_record.storage_index;

    lv_timer_t *timer = lv_timer_create(rename_save_timer_cb, 1, NULL);
    (void)timer;
}

static void detail_rename_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    if (rename_screen) {
        lv_obj_del_async(rename_screen);
        rename_screen = NULL;
        rename_textarea = NULL;
    }

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
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_back, 18, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
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
    lv_textarea_set_text(rename_textarea, detail_record.model_name);
    lv_textarea_set_placeholder_text(rename_textarea, "Model Name");
    lv_obj_set_size(rename_textarea, 220, 45);
    lv_obj_align(rename_textarea, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(rename_textarea, lv_color_hex(0x212121), 0);
    lv_obj_set_style_bg_opa(rename_textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(rename_textarea, lv_color_white(), 0);
    lv_obj_set_style_radius(rename_textarea, 18, 0);
    apply_light_style(rename_textarea);

    lv_obj_t *btn_save = lv_btn_create(rename_screen);
    lv_obj_set_size(btn_save, 220, 45);
    lv_obj_align(btn_save, LV_ALIGN_TOP_MID, 0, 135);
    lv_obj_set_style_bg_color(btn_save, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(btn_save, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_save, 18, 0);
    lv_obj_set_style_border_width(btn_save, 0, 0);
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

static void open_detail_page(history_record_t *record)
{
    if (!record) return;

    // Save record copy
    detail_record = *record;

    // Destroy previous detail if any
    if (screen_detail) {
        lv_obj_del_async(screen_detail);
        screen_detail = NULL;
    }

    screen_detail = lv_obj_create(NULL);
    lv_obj_set_size(screen_detail, 240, 320);
    lv_obj_set_scrollbar_mode(screen_detail, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(screen_detail, lv_color_hex(0xF0F4F8), 0);
    lv_obj_set_style_bg_opa(screen_detail, LV_OPA_COVER, 0);
    apply_light_style(screen_detail);

    // Back button (top-left)
    lv_obj_t *btn_back = lv_btn_create(screen_detail);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_back, 18, 0);
    lv_obj_set_style_border_width(btn_back, 0, 0);
    apply_light_style(btn_back);
    lv_obj_add_event_cb(btn_back, detail_back_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);

    // Delete button (top-right, red)
    lv_obj_t *btn_delete = lv_btn_create(screen_detail);
    lv_obj_set_size(btn_delete, 70, 35);
    lv_obj_set_pos(btn_delete, 162, 7);
    lv_obj_set_style_bg_color(btn_delete, lv_color_hex(0xE53935), 0);
    lv_obj_set_style_bg_opa(btn_delete, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_delete, 8, 0);
    lv_obj_set_style_border_width(btn_delete, 0, 0);
    apply_light_style(btn_delete);
    lv_obj_add_event_cb(btn_delete, detail_delete_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *del_label = lv_label_create(btn_delete);
    lv_label_set_text(del_label, "Delete");
    lv_obj_set_style_text_color(del_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(del_label, &lv_font_montserrat_12, 0);
    lv_obj_center(del_label);

    // Content area (card style)
    lv_obj_t *card = lv_obj_create(screen_detail);
    lv_obj_set_size(card, 224, 240);
    lv_obj_set_pos(card, 8, 55);
    lv_obj_set_style_bg_color(card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    apply_light_style(card);

    // Model name (title)
    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, record->model_name);
    lv_obj_set_width(name, 196);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0x1A237E), 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

    // Time
    lv_obj_t *time_label = lv_label_create(card);
    lv_label_set_text_fmt(time_label, "Time: %s", record->created_time);
    lv_obj_set_width(time_label, 196);
    lv_label_set_long_mode(time_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time_label, lv_color_hex(0x6B7280), 0);
    lv_obj_align(time_label, LV_ALIGN_TOP_LEFT, 0, 30);

    // Model size
    lv_obj_t *model_label = lv_label_create(card);
    lv_label_set_text_fmt(model_label, "Model: %s", record->model_size);
    lv_obj_set_width(model_label, 196);
    lv_label_set_long_mode(model_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(model_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(model_label, lv_color_hex(0x4B5563), 0);
    lv_obj_align(model_label, LV_ALIGN_TOP_LEFT, 0, 55);

    // Pointcloud size
    lv_obj_t *point_label = lv_label_create(card);
    lv_label_set_text_fmt(point_label, "Point: %s", record->pointcloud_size);
    lv_obj_set_width(point_label, 196);
    lv_label_set_long_mode(point_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(point_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(point_label, lv_color_hex(0x4B5563), 0);
    lv_obj_align(point_label, LV_ALIGN_TOP_LEFT, 0, 75);

    // Path
    lv_obj_t *path_title = lv_label_create(card);
    lv_label_set_text(path_title, "Path:");
    lv_obj_set_style_text_font(path_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(path_title, lv_color_hex(0x9CA3AF), 0);
    lv_obj_align(path_title, LV_ALIGN_TOP_LEFT, 0, 100);

    lv_obj_t *path_label = lv_label_create(card);
    lv_label_set_text(path_label, record->model_path);
    lv_obj_set_width(path_label, 196);
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(path_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(path_label, lv_color_hex(0x9CA3AF), 0);
    lv_obj_align(path_label, LV_ALIGN_TOP_LEFT, 0, 118);

    // Rename button (centered below card)
    lv_obj_t *btn_rename = lv_btn_create(screen_detail);
    lv_obj_set_size(btn_rename, 140, 40);
    lv_obj_set_pos(btn_rename, 50, 270);
    lv_obj_set_style_bg_color(btn_rename, lv_color_hex(0x2195F6), 0);
    lv_obj_set_style_bg_opa(btn_rename, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_rename, 10, 0);
    lv_obj_set_style_border_width(btn_rename, 0, 0);
    apply_light_style(btn_rename);
    lv_obj_add_event_cb(btn_rename, detail_rename_callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rename_btn_label = lv_label_create(btn_rename);
    lv_label_set_text(rename_btn_label, LV_SYMBOL_EDIT " Rename");
    lv_obj_set_style_text_color(rename_btn_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(rename_btn_label, &lv_font_montserrat_14, 0);
    lv_obj_center(rename_btn_label);

    lv_scr_load(screen_detail);
}

// ============== History list ==============

static void history_row_callback(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        history_record_t *record = (history_record_t *)lv_event_get_user_data(e);
        open_detail_page(record);
    }
}

static void add_history_row(lv_obj_t *parent, history_record_t *record)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, 208);
    lv_obj_set_height(row, 44);
    lv_obj_set_style_bg_color(row, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_shadow_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, history_row_callback, LV_EVENT_CLICKED, record);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, record->model_name);
    lv_obj_set_width(name, 188);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0x1F2937), 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *time = lv_label_create(row);
    lv_label_set_text(time, record->created_time);
    lv_obj_set_width(time, 188);
    lv_label_set_long_mode(time, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(time, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(time, lv_color_hex(0x6B7280), 0);
    lv_obj_align(time, LV_ALIGN_TOP_LEFT, 0, 22);
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
        for (size_t i = 0; i < 10; i++) {
            if (history_test_records[i].model_name[0] != '\0') {
                history_records[count] = history_test_records[i];
                count++;
            }
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
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back_btn, 18, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    apply_light_style(back_btn);
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
    lv_obj_set_style_pad_row(list, 12, 0);
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
