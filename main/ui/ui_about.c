/**
 * 关于页面
 */

#include "lvgl.h"
#include "ui_about.h"
#include "ui_settings.h"

static lv_obj_t *screen_about = NULL;

static void btn_back_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        lv_scr_load(ui_settings_get_screen());
    }
}

void ui_about_create(void)
{
    // 删除旧的screen（如果存在），防止内存泄漏
    if(screen_about != NULL) {
        lv_obj_del(screen_about);
        screen_about = NULL;
    }

    screen_about = lv_obj_create(NULL);
    lv_obj_set_size(screen_about, 240, 320);
    lv_obj_set_scrollbar_mode(screen_about, LV_SCROLLBAR_MODE_OFF);

    lv_obj_set_style_bg_color(screen_about, lv_color_hex(0x667EEA), 0);
    lv_obj_set_style_bg_grad_color(screen_about, lv_color_hex(0x764BA2), 0);
    lv_obj_set_style_bg_grad_dir(screen_about, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(screen_about, LV_OPA_COVER, 0);

    // 返回按钮 (统一样式)
    lv_obj_t *btn_back = lv_btn_create(screen_about);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x4557C3), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, btn_back_callback, LV_EVENT_ALL, NULL);

    // 图标
    lv_obj_t *logo = lv_label_create(screen_about);
    lv_label_set_text(logo, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(logo, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(logo, lv_color_white(), 0);
    lv_obj_align(logo, LV_ALIGN_TOP_MID, 0, 75);

    // 应用名称
    lv_obj_t *app_name = lv_label_create(screen_about);
    lv_label_set_text(app_name, "Smart Control");
    lv_obj_set_style_text_font(app_name, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(app_name, lv_color_white(), 0);
    lv_obj_align(app_name, LV_ALIGN_TOP_MID, 0, 135);

    // 版本
    lv_obj_t *version = lv_label_create(screen_about);
    lv_label_set_text(version, "Version 1.0.0");
    lv_obj_set_style_text_font(version, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(version, lv_color_hex(0xDDDDDD), 0);
    lv_obj_align(version, LV_ALIGN_TOP_MID, 0, 165);

    // 分隔线
    lv_obj_t *divider = lv_obj_create(screen_about);
    lv_obj_set_size(divider, 200, 2);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 195);
    lv_obj_set_style_bg_color(divider, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_50, 0);
    lv_obj_set_style_radius(divider, 1, 0);

    // 制作信息
    lv_obj_t *credits = lv_label_create(screen_about);
    lv_label_set_text(credits, "Powered by Liu Senyi");
    lv_obj_set_style_text_font(credits, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(credits, lv_color_white(), 0);
    lv_obj_set_style_text_align(credits, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(credits, LV_ALIGN_BOTTOM_MID, 0, -30);

    lv_scr_load(screen_about);
}
