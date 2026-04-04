/**
 * 温度仪表盘页面 - 使用GXHTC3传感器读取实时温度
 * 添加湿度仪表盘，向下滑动查看
 */

#include "lvgl.h"
#include "ui_temp.h"
#include "ui_home.h"
#include "gxhtc3.h"
#include <stdio.h>

static lv_obj_t *screen_temp = NULL;
static lv_timer_t *temp_timer = NULL;

// 温度仪表盘组件
static lv_obj_t *meter_temp = NULL;
static lv_meter_indicator_t *indic_needle_temp = NULL;
static lv_obj_t *label_temp = NULL;

// 湿度仪表盘组件
static lv_obj_t *meter_humi = NULL;
static lv_meter_indicator_t *indic_needle_humi = NULL;
static lv_obj_t *label_humi = NULL;

// 外部变量来自gxhtc3.c
extern float temp;
extern float humi;

static void temp_read_timer_cb(lv_timer_t *t)
{
    // 读取GXHTC3传感器数据
    esp_err_t ret = gxhtc3_get_tah();

    float temp_value = 25.0f;  // 默认值
    float humi_value = 50.0f;  // 默认值

    if(ret == ESP_OK) {
        temp_value = temp;
        humi_value = humi;
    }

    // 更新温度仪表盘指针
    if(meter_temp && indic_needle_temp) {
        // 限制范围 0-50
        if(temp_value < 0) temp_value = 0;
        if(temp_value > 50) temp_value = 50;
        lv_meter_set_indicator_value(meter_temp, indic_needle_temp, (int32_t)temp_value);
    }

    // 更新温度标签
    if(label_temp) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f°C", temp_value);
        lv_label_set_text(label_temp, buf);
    }

    // 更新湿度仪表盘指针
    if(meter_humi && indic_needle_humi) {
        // 限制范围 0-100
        if(humi_value < 0) humi_value = 0;
        if(humi_value > 100) humi_value = 100;
        lv_meter_set_indicator_value(meter_humi, indic_needle_humi, (int32_t)humi_value);
    }

    // 更新湿度标签
    if(label_humi) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f%%", humi_value);
        lv_label_set_text(label_humi, buf);
    }
}

static void btn_back_callback(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        if(temp_timer) {
            lv_timer_del(temp_timer);
            temp_timer = NULL;
        }
        lv_scr_load(ui_home_get_screen());
    }
}

// 内部函数：创建温度页面UI，使用指定的初始温度值
static void ui_temp_create_internal(float initial_temp, float initial_humi)
{
    // 删除旧的screen（如果存在），防止内存泄漏
    if(screen_temp != NULL) {
        lv_obj_del(screen_temp);
        screen_temp = NULL;
        meter_temp = NULL;
        indic_needle_temp = NULL;
        label_temp = NULL;
        meter_humi = NULL;
        indic_needle_humi = NULL;
        label_humi = NULL;
        temp_timer = NULL;
    }

    screen_temp = lv_obj_create(NULL);
    // 增加高度以支持滚动 - 320 -> 640
    lv_obj_set_size(screen_temp, 240, 640);
    lv_obj_set_scrollbar_mode(screen_temp, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(screen_temp, lv_color_hex(0x0D1117), 0);
    lv_obj_set_style_bg_opa(screen_temp, LV_OPA_COVER, 0);
    // 启用滚动
    lv_obj_set_scroll_dir(screen_temp, LV_DIR_VER);

    // 返回按钮 (统一样式: radius=18)
    lv_obj_t *btn_back = lv_btn_create(screen_temp);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 5, 5);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_radius(btn_back, 18, 0);

    lv_obj_t *back_label = lv_label_create(btn_back);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(back_label, &lv_font_montserrat_16, 0);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(btn_back, btn_back_callback, LV_EVENT_ALL, NULL);

    // ========== 温度仪表盘区域 ==========
    // 温度标题
    lv_obj_t *title_temp = lv_label_create(screen_temp);
    lv_label_set_text(title_temp, "Temperature");
    lv_obj_set_style_text_font(title_temp, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_temp, lv_color_hex(0x8B949E), 0);
    lv_obj_align(title_temp, LV_ALIGN_TOP_MID, 0, 55);

    // 创建温度仪表盘
    meter_temp = lv_meter_create(screen_temp);
    lv_obj_set_size(meter_temp, 200, 200);
    lv_obj_align(meter_temp, LV_ALIGN_TOP_MID, 0, 75);
    lv_obj_set_style_bg_opa(meter_temp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meter_temp, 0, 0);
    lv_obj_set_style_pad_all(meter_temp, 0, 0);

    // 添加温度刻度
    lv_meter_scale_t *scale_temp = lv_meter_add_scale(meter_temp);
    lv_meter_set_scale_ticks(meter_temp, scale_temp, 11, 2, 10, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter_temp, scale_temp, 1, 2, 15, lv_palette_main(LV_PALETTE_GREY), 10);

    // 设置温度刻度范围 0-50度，从135度开始，270度跨度
    lv_meter_set_scale_range(meter_temp, scale_temp, 0, 50, 270, 135);

    // 创建温度渐变 - 0度深蓝色到50度深红色
    for(int i = 0; i <= 50; i++) {
        float ratio = (float)i / 50.0f;
        lv_color_t color;

        if(ratio < 0.25f) {
            // 0-12.5度: 深蓝色->浅蓝色
            float r = ratio / 0.25f;
            color = lv_color_make(
                (uint8_t)(13 + (100 - 13) * r),
                (uint8_t)(71 + (181 - 71) * r),
                (uint8_t)(161 + (246 - 161) * r)
            );
        } else if(ratio < 0.5f) {
            // 12.5-25度: 浅蓝色->浅绿色
            float r = (ratio - 0.25f) / 0.25f;
            color = lv_color_make(
                (uint8_t)(100 + (129 - 100) * r),
                (uint8_t)(181 + (199 - 181) * r),
                (uint8_t)(246 + (132 - 246) * r)
            );
        } else if(ratio < 0.75f) {
            // 25-37.5度: 浅绿色->浅黄色
            float r = (ratio - 0.5f) / 0.25f;
            color = lv_color_make(
                (uint8_t)(129 + (255 - 129) * r),
                (uint8_t)(199 + (245 - 199) * r),
                (uint8_t)(132 + (157 - 132) * r)
            );
        } else {
            // 37.5-50度: 浅黄色->深红色
            float r = (ratio - 0.75f) / 0.25f;
            color = lv_color_make(
                (uint8_t)(255 + (198 - 255) * r),
                (uint8_t)(245 + (40 - 245) * r),
                (uint8_t)(157 + (40 - 157) * r)
            );
        }

        // 每度创建一个渐变段
        if(i < 50) {
            lv_meter_indicator_t *indic = lv_meter_add_arc(meter_temp, scale_temp, 10, color, 0);
            lv_meter_set_indicator_start_value(meter_temp, indic, i);
            lv_meter_set_indicator_end_value(meter_temp, indic, i + 1);
        }
    }

    // 添加温度指针
    indic_needle_temp = lv_meter_add_needle_line(meter_temp, scale_temp, 4, lv_palette_main(LV_PALETTE_RED), -10);
    lv_meter_set_indicator_value(meter_temp, indic_needle_temp, (int32_t)initial_temp);

    // 温度中心圆点
    lv_obj_t *pivot_temp = lv_obj_create(screen_temp);
    lv_obj_set_size(pivot_temp, 12, 12);
    lv_obj_align(pivot_temp, LV_ALIGN_TOP_MID, 0, 175);
    lv_obj_set_style_bg_color(pivot_temp, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_set_style_bg_opa(pivot_temp, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pivot_temp, 6, 0);
    lv_obj_set_style_border_width(pivot_temp, 0, 0);

    // 温度值显示
    label_temp = lv_label_create(screen_temp);
    char temp_buf[32];
    snprintf(temp_buf, sizeof(temp_buf), "%.1f°C", initial_temp);
    lv_label_set_text(label_temp, temp_buf);
    lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_temp, lv_color_hex(0xC9D1D9), 0);
    lv_obj_align(label_temp, LV_ALIGN_TOP_MID, 0, 205);

    // 温度单位标签
    lv_obj_t *unit_label_temp = lv_label_create(screen_temp);
    lv_label_set_text(unit_label_temp, "Celsius");
    lv_obj_set_style_text_font(unit_label_temp, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(unit_label_temp, lv_color_hex(0x8B949E), 0);
    lv_obj_align(unit_label_temp, LV_ALIGN_TOP_MID, 0, 230);

    // 分隔线
    lv_obj_t *divider = lv_obj_create(screen_temp);
    lv_obj_set_size(divider, 200, 2);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 260);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x30363D), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_50, 0);
    lv_obj_set_style_radius(divider, 1, 0);
    lv_obj_set_style_border_width(divider, 0, 0);

    // ========== 湿度仪表盘区域 ==========
    // 湿度标题
    lv_obj_t *title_humi = lv_label_create(screen_temp);
    lv_label_set_text(title_humi, "Humidity");
    lv_obj_set_style_text_font(title_humi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_humi, lv_color_hex(0x8B949E), 0);
    lv_obj_align(title_humi, LV_ALIGN_TOP_MID, 0, 285);

    // 创建湿度仪表盘
    meter_humi = lv_meter_create(screen_temp);
    lv_obj_set_size(meter_humi, 200, 200);
    lv_obj_align(meter_humi, LV_ALIGN_TOP_MID, 0, 305);
    lv_obj_set_style_bg_opa(meter_humi, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(meter_humi, 0, 0);
    lv_obj_set_style_pad_all(meter_humi, 0, 0);

    // 添加湿度刻度
    lv_meter_scale_t *scale_humi = lv_meter_add_scale(meter_humi);
    lv_meter_set_scale_ticks(meter_humi, scale_humi, 11, 2, 10, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(meter_humi, scale_humi, 1, 2, 15, lv_palette_main(LV_PALETTE_GREY), 10);

    // 设置湿度刻度范围 0-100%，从135度开始，270度跨度
    lv_meter_set_scale_range(meter_humi, scale_humi, 0, 100, 270, 135);

    // 创建湿度渐变 - 0%深红色到100%深蓝色（与温度相反）
    for(int i = 0; i <= 100; i++) {
        float ratio = (float)i / 100.0f;
        lv_color_t color;

        if(ratio < 0.25f) {
            // 0-25%: 深红色->浅红色
            float r = ratio / 0.25f;
            color = lv_color_make(
                (uint8_t)(198 + (255 - 198) * r),
                (uint8_t)(40 + (100 - 40) * r),
                (uint8_t)(40 + (100 - 40) * r)
            );
        } else if(ratio < 0.5f) {
            // 25-50%: 浅红色->浅黄色
            float r = (ratio - 0.25f) / 0.25f;
            color = lv_color_make(
                (uint8_t)(255 + (255 - 255) * r),
                (uint8_t)(100 + (245 - 100) * r),
                (uint8_t)(100 + (157 - 100) * r)
            );
        } else if(ratio < 0.75f) {
            // 50-75%: 浅黄色->浅绿色
            float r = (ratio - 0.5f) / 0.25f;
            color = lv_color_make(
                (uint8_t)(255 + (129 - 255) * r),
                (uint8_t)(245 + (199 - 245) * r),
                (uint8_t)(157 + (132 - 157) * r)
            );
        } else {
            // 75-100%: 浅绿色->深蓝色
            float r = (ratio - 0.75f) / 0.25f;
            color = lv_color_make(
                (uint8_t)(129 + (13 - 129) * r),
                (uint8_t)(199 + (71 - 199) * r),
                (uint8_t)(132 + (161 - 132) * r)
            );
        }

        // 每个百分点创建一个渐变段
        if(i < 100) {
            lv_meter_indicator_t *indic = lv_meter_add_arc(meter_humi, scale_humi, 10, color, 0);
            lv_meter_set_indicator_start_value(meter_humi, indic, i);
            lv_meter_set_indicator_end_value(meter_humi, indic, i + 1);
        }
    }

    // 添加湿度指针（蓝色）
    indic_needle_humi = lv_meter_add_needle_line(meter_humi, scale_humi, 4, lv_palette_main(LV_PALETTE_BLUE), -10);
    lv_meter_set_indicator_value(meter_humi, indic_needle_humi, (int32_t)initial_humi);

    // 湿度中心圆点（蓝色）
    lv_obj_t *pivot_humi = lv_obj_create(screen_temp);
    lv_obj_set_size(pivot_humi, 12, 12);
    lv_obj_align(pivot_humi, LV_ALIGN_TOP_MID, 0, 405);
    lv_obj_set_style_bg_color(pivot_humi, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(pivot_humi, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pivot_humi, 6, 0);
    lv_obj_set_style_border_width(pivot_humi, 0, 0);

    // 湿度值显示
    label_humi = lv_label_create(screen_temp);
    char humi_buf[32];
    snprintf(humi_buf, sizeof(humi_buf), "%.1f%%", initial_humi);
    lv_label_set_text(label_humi, humi_buf);
    lv_obj_set_style_text_font(label_humi, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_humi, lv_color_hex(0xC9D1D9), 0);
    lv_obj_align(label_humi, LV_ALIGN_TOP_MID, 0, 435);

    // 湿度单位标签
    lv_obj_t *unit_label_humi = lv_label_create(screen_temp);
    lv_label_set_text(unit_label_humi, "Percent");
    lv_obj_set_style_text_font(unit_label_humi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(unit_label_humi, lv_color_hex(0x8B949E), 0);
    lv_obj_align(unit_label_humi, LV_ALIGN_TOP_MID, 0, 460);

    // 定时器 - 每500ms读取温湿度
    temp_timer = lv_timer_create(temp_read_timer_cb, 500, NULL);

    lv_scr_load(screen_temp);
}

// 使用预测量温度和湿度创建页面
void ui_temp_create_with_temp(float initial_temp)
{
    // 读取初始湿度
    float initial_humi = 50.0f;
    gxhtc3_get_tah();
    initial_humi = humi;
    if(initial_humi < 0) initial_humi = 0;
    if(initial_humi > 100) initial_humi = 100;

    ui_temp_create_internal(initial_temp, initial_humi);
}

void ui_temp_create(void)
{
    // 先读取传感器温度和湿度
    float temp_value = 25.0f;
    float humi_value = 50.0f;
    esp_err_t ret = gxhtc3_get_tah();
    if(ret == ESP_OK) {
        temp_value = temp;
        humi_value = humi;
    }
    // 限制范围
    if(temp_value < 0) temp_value = 0;
    if(temp_value > 50) temp_value = 50;
    if(humi_value < 0) humi_value = 0;
    if(humi_value > 100) humi_value = 100;

    ui_temp_create_internal(temp_value, humi_value);
}
