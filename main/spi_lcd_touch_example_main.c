/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lvgl.h"
#include "myi2c.h"
#include "ui_home.h"
// #include "gui_guider.h"
// #include "lv_demos.h"

#if CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9341
#include "esp_lcd_ili9341.h"
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
#include "esp_lcd_gc9a01.h"
#endif

#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_STMPE610
#include "esp_lcd_touch_stmpe610.h"
#elif CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_FT6336
#include "esp_lcd_touch_ft5x06.h"
#endif

static const char *TAG = "example";

// Using SPI2 in the example
#define LCD_HOST  SPI2_HOST

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ     (80 * 1000 * 1000)  // 提高到80MHz (ESP32-C3最大频率)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  0
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_PIN_NUM_SCLK           3
#define EXAMPLE_PIN_NUM_MOSI           5
#define EXAMPLE_PIN_NUM_MISO           -1
#define EXAMPLE_PIN_NUM_LCD_DC         6
#define EXAMPLE_PIN_NUM_LCD_RST        -1
#define EXAMPLE_PIN_NUM_LCD_CS         4
#define EXAMPLE_PIN_NUM_BK_LIGHT       2
#define EXAMPLE_PIN_NUM_TOUCH_CS       -1

// The pixel number in horizontal and vertical
#if CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9341
#define EXAMPLE_LCD_H_RES              240
#define EXAMPLE_LCD_V_RES              320
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
#define EXAMPLE_LCD_H_RES              240
#define EXAMPLE_LCD_V_RES              240
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_ST7789
#define EXAMPLE_LCD_H_RES              240
#define EXAMPLE_LCD_V_RES              320
#endif
// Bit number used to represent command and parameter
#define EXAMPLE_LCD_CMD_BITS           8
#define EXAMPLE_LCD_PARAM_BITS         8

#define EXAMPLE_LVGL_TICK_PERIOD_MS    1


#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
esp_lcd_touch_handle_t tp = NULL;
#endif

// WiFi connection state
static bool wifi_connected = false;
static lv_obj_t *wifi_status_label = NULL;
static int last_disconnect_reason = 0;
static int auto_reconnect_count = 0;  // Auto-retry counter
static bool allow_auto_reconnect = false;  // Control when to allow reconnect
static bool is_manual_connect = false;  // Track if this is a manual connect attempt

// LCD panel handle (全局，供视频直接写屏使用)
static esp_lcd_panel_handle_t s_panel_handle = NULL;

// 直接写LCD接口（绕过LVGL，用于实时视频）
void app_lcd_draw_bitmap(int x1, int y1, int x2, int y2, const void *data)
{
    if (s_panel_handle) {
        esp_lcd_panel_draw_bitmap(s_panel_handle, x1, y1, x2, y2, data);
    }
}

// 获取panel handle
void *app_lcd_get_panel(void)
{
    return s_panel_handle;
}

// NVS keys - Support multiple WiFi configs
#define NVS_NAMESPACE "wifi_config"
#define MAX_SAVED_NETWORKS 5
#define NVS_KEY_SSID_PREFIX "ssid_"
#define NVS_KEY_PASSWORD_PREFIX "pwd_"
#define NVS_KEY_RSSI_PREFIX "rssi_"
#define NVS_KEY_COUNT "count"

// Save WiFi config to NVS with RSSI (support multiple networks)
static esp_err_t save_wifi_config(const char *ssid, const char *password, int32_t rssi)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Check if this SSID already exists
    int existing_index = -1;
    int32_t count = 0;
    nvs_get_i32(nvs_handle, NVS_KEY_COUNT, &count);

    char key_ssid[32], key_pwd[32], key_rssi[32];
    char saved_ssid[33];
    size_t saved_ssid_len = sizeof(saved_ssid);

    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        snprintf(key_ssid, sizeof(key_ssid), "%s%d", NVS_KEY_SSID_PREFIX, i);
        err = nvs_get_str(nvs_handle, key_ssid, saved_ssid, &saved_ssid_len);
        if (err == ESP_OK && strcmp(saved_ssid, ssid) == 0) {
            existing_index = i;
            break;
        }
        saved_ssid_len = sizeof(saved_ssid);  // Reset for next iteration
    }

    // If SSID exists, update it; otherwise add as new
    int index;
    if (existing_index >= 0) {
        index = existing_index;
        ESP_LOGI(TAG, "Updating existing WiFi config at index %d", index);
    } else if (count < MAX_SAVED_NETWORKS) {
        index = count;
        count++;
        nvs_set_i32(nvs_handle, NVS_KEY_COUNT, count);
        ESP_LOGI(TAG, "Adding new WiFi config at index %d", index);
    } else {
        // Find weakest signal to replace
        int32_t min_rssi = INT32_MAX;
        int min_index = 0;
        for (int i = 0; i < MAX_SAVED_NETWORKS; i++) {
            snprintf(key_rssi, sizeof(key_rssi), "%s%d", NVS_KEY_RSSI_PREFIX, i);
            int32_t saved_rssi;
            if (nvs_get_i32(nvs_handle, key_rssi, &saved_rssi) == ESP_OK && saved_rssi < min_rssi) {
                min_rssi = saved_rssi;
                min_index = i;
            }
        }
        index = min_index;
        ESP_LOGI(TAG, "Replacing weakest WiFi config at index %d", index);
    }

    // Save SSID, password, and RSSI
    snprintf(key_ssid, sizeof(key_ssid), "%s%d", NVS_KEY_SSID_PREFIX, index);
    snprintf(key_pwd, sizeof(key_pwd), "%s%d", NVS_KEY_PASSWORD_PREFIX, index);
    snprintf(key_rssi, sizeof(key_rssi), "%s%d", NVS_KEY_RSSI_PREFIX, index);

    err = nvs_set_str(nvs_handle, key_ssid, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save SSID: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, key_pwd, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save password: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_i32(nvs_handle, key_rssi, rssi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save RSSI: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "WiFi config saved to NVS (SSID: %s, RSSI: %d)", ssid, (int)rssi);
    }
    return err;
}

// Load best WiFi config from NVS (strongest signal)
static esp_err_t load_best_wifi_config(char *ssid, size_t ssid_len, char *password, size_t password_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No saved WiFi config found");
        return err;
    }

    int32_t count = 0;
    err = nvs_get_i32(nvs_handle, NVS_KEY_COUNT, &count);
    if (err != ESP_OK || count == 0) {
        ESP_LOGW(TAG, "No WiFi networks saved");
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    // Find network with strongest signal (highest RSSI)
    int best_index = -1;
    int32_t best_rssi = INT32_MIN;
    char key_rssi[32];

    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        snprintf(key_rssi, sizeof(key_rssi), "%s%d", NVS_KEY_RSSI_PREFIX, i);
        int32_t rssi;
        if (nvs_get_i32(nvs_handle, key_rssi, &rssi) == ESP_OK && rssi > best_rssi) {
            best_rssi = rssi;
            best_index = i;
        }
    }

    if (best_index < 0) {
        ESP_LOGW(TAG, "No valid WiFi config found");
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }

    // Load the best network
    char key_ssid[32], key_pwd[32];
    snprintf(key_ssid, sizeof(key_ssid), "%s%d", NVS_KEY_SSID_PREFIX, best_index);
    snprintf(key_pwd, sizeof(key_pwd), "%s%d", NVS_KEY_PASSWORD_PREFIX, best_index);

    size_t required_size = 0;

    // Get SSID
    err = nvs_get_str(nvs_handle, key_ssid, NULL, &required_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No SSID saved at index %d", best_index);
        nvs_close(nvs_handle);
        return err;
    }
    if (required_size > ssid_len) {
        ESP_LOGE(TAG, "SSID too long");
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }
    nvs_get_str(nvs_handle, key_ssid, ssid, &required_size);

    // Get password
    err = nvs_get_str(nvs_handle, key_pwd, NULL, &required_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No password saved at index %d", best_index);
        nvs_close(nvs_handle);
        return err;
    }
    if (required_size > password_len) {
        ESP_LOGE(TAG, "Password too long");
        nvs_close(nvs_handle);
        return ESP_ERR_NO_MEM;
    }
    nvs_get_str(nvs_handle, key_pwd, password, &required_size);

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Best WiFi config loaded from NVS: %s (RSSI: %d)", ssid, (int)best_rssi);
    return ESP_OK;
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi STA started");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi CONNECTED event received!");
        wifi_connected = true;
        auto_reconnect_count = 0;  // Reset retry counter on successful connection
        is_manual_connect = false;  // Clear manual connect flag
        if(wifi_status_label) {
            lv_label_set_text(wifi_status_label, LV_SYMBOL_WIFI);
            lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0x4CAF50), 0);
            lv_obj_clear_flag(wifi_status_label, LV_OBJ_FLAG_HIDDEN);  // Show WiFi icon
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        ESP_LOGW(TAG, "WiFi DISCONNECTED event, reason=%d", event->reason);
        last_disconnect_reason = event->reason;

        // Log reason description
        const char* reason_str = "";
        switch(event->reason) {
            case 1: reason_str = "UNSPECIFIED"; break;
            case 2: reason_str = "AUTH_EXPIRE"; break;
            case 3: reason_str = "AUTH_LEAVE"; break;
            case 4: reason_str = "ASSOC_EXPIRE"; break;
            case 5: reason_str = "ASSOC_TOOMANY"; break;
            case 6: reason_str = "NOT_AUTHED"; break;
            case 7: reason_str = "NOT_ASSOCED"; break;
            case 8: reason_str = "ASSOC_LEAVE"; break;
            case 9: reason_str = "ASSOC_NOT_AUTHED"; break;
            case 10: reason_str = "DISASSOC_PWRCAP_BAD"; break;
            case 11: reason_str = "DISASSOC_SUPCHAN_BAD"; break;
            case 12: reason_str = "IE_INVALID"; break;
            case 13: reason_str = "MIC_FAILURE"; break;
            case 14: reason_str = "4WAY_HANDSHAKE_TIMEOUT"; break;
            case 15: reason_str = "GROUP_KEY_UPDATE_TIMEOUT"; break;
            case 16: reason_str = "IE_IN_4WAY_DIFFERS"; break;
            case 17: reason_str = "GROUP_CIPHER_INVALID"; break;
            case 18: reason_str = "PAIRWISE_CIPHER_INVALID"; break;
            case 19: reason_str = "AKMP_INVALID"; break;
            case 20: reason_str = "UNSUPP_RSN_IE_CAPAB"; break;
            case 21: reason_str = "INVALID_RSN_IE_CAPAB"; break;
            case 22: reason_str = "802_1X_AUTH_FAILED"; break;
            case 23: reason_str = "CIPHER_SUITE_REJECTED"; break;
            case 24: reason_str = "BEACON_TIMEOUT"; break;
            case 200: reason_str = "NO_AP_FOUND"; break;
            case 201: reason_str = "AUTH_FAIL"; break;
            case 202: reason_str = "ASSOC_FAIL"; break;
            case 203: reason_str = "HANDSHAKE_TIMEOUT"; break;
            default: reason_str = "UNKNOWN"; break;
        }
        ESP_LOGW(TAG, "Disconnect reason: %s", reason_str);

        wifi_connected = false;
        if(wifi_status_label) {
            lv_obj_add_flag(wifi_status_label, LV_OBJ_FLAG_HIDDEN);  // Hide WiFi icon
        }

        // Auto-reconnect logic based on allow_auto_reconnect flag and retry count
        if(event->reason != 8 && allow_auto_reconnect) {  // Not manually disconnected
            auto_reconnect_count++;
            int max_retries = is_manual_connect ? 1 : 3;  // Manual: 1 retry, Auto: 3 retries

            if(auto_reconnect_count <= max_retries) {
                ESP_LOGI(TAG, "Attempting to reconnect (%d/%d)...", auto_reconnect_count, max_retries);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Max reconnect attempts reached. Giving up.");
                allow_auto_reconnect = false;  // Disable further auto-reconnect
                auto_reconnect_count = 0;
                is_manual_connect = false;
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));

        // Save WiFi config on successful connection (got IP) with current RSSI
        wifi_config_t wifi_config;
        wifi_ap_record_t ap_info;
        int32_t rssi = -100;  // Default weak signal if we can't get RSSI

        if(esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
            ESP_LOGI(TAG, "Current RSSI: %d", (int)rssi);
        }

        if(esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
            save_wifi_config((char*)wifi_config.sta.ssid, (char*)wifi_config.sta.password, rssi);
        }
    }
}

// Set WiFi status label for home screen
void app_main_set_wifi_status_label(lv_obj_t *label)
{
    wifi_status_label = label;
}

// Check if WiFi is connected
bool app_main_is_wifi_connected(void)
{
    return wifi_connected;
}

// Get last disconnect reason
int get_last_disconnect_reason(void)
{
    return last_disconnect_reason;
}

// Get currently connected SSID (returns empty string if not connected)
void app_main_get_connected_ssid(char *ssid, size_t max_len)
{
    if(wifi_connected) {
        wifi_config_t wifi_config;
        if(esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
            strncpy(ssid, (char*)wifi_config.sta.ssid, max_len - 1);
            ssid[max_len - 1] = '\0';
            return;
        }
    }
    ssid[0] = '\0';
}

// Disconnect from WiFi
void app_main_wifi_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting WiFi...");
    esp_wifi_disconnect();
}

// Trigger auto-connect to saved WiFi (boot time, limited retries)
void app_main_auto_connect_wifi(void)
{
    char ssid[33] = {0};
    char password[64] = {0};

    if(load_best_wifi_config(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        ESP_LOGI(TAG, "Auto-connecting to best saved WiFi: %s", ssid);

        wifi_config_t wifi_config = {0};
        memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
        memcpy(wifi_config.sta.password, password, strlen(password));

        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if(err == ESP_OK) {
            allow_auto_reconnect = true;  // Enable auto-reconnect with retry limit
            auto_reconnect_count = 0;
            is_manual_connect = false;
            esp_wifi_connect();
            ESP_LOGI(TAG, "Auto-connect started (max 3 retries)");
        } else {
            ESP_LOGE(TAG, "Auto-connect config failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "No saved WiFi config, skipping auto-connect");
    }
}

// Trigger WiFi connect from UI (WiFi settings page, 1 retry only)
void app_main_trigger_wifi_connect(void)
{
    char ssid[33] = {0};
    char password[64] = {0};

    if(load_best_wifi_config(ssid, sizeof(ssid), password, sizeof(password)) == ESP_OK) {
        ESP_LOGI(TAG, "Manual connect to best saved WiFi: %s", ssid);

        wifi_config_t wifi_config = {0};
        memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
        memcpy(wifi_config.sta.password, password, strlen(password));

        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if(err == ESP_OK) {
            allow_auto_reconnect = true;  // Enable auto-reconnect with retry limit
            auto_reconnect_count = 0;
            is_manual_connect = true;  // Mark as manual connect (1 retry only)
            esp_wifi_connect();
            ESP_LOGI(TAG, "Manual connect started (max 1 retry)");
        } else {
            ESP_LOGE(TAG, "Manual connect config failed: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGI(TAG, "No saved WiFi config available");
    }
}

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

/* Rotate display and touch, when rotated screen in LVGL. Called when driver parameters are updated. */
static void example_lvgl_port_update_callback(lv_disp_drv_t *drv)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;

    switch (drv->rotated) {
    case LV_DISP_ROT_270:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, false, false);
#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
        // Rotate LCD touch
        esp_lcd_touch_set_mirror_y(tp, true);
        esp_lcd_touch_set_mirror_x(tp, false);
#endif
        break;
    case LV_DISP_ROT_NONE:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, false, true);
#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
        // Rotate LCD touch
        esp_lcd_touch_set_mirror_y(tp, true);
        esp_lcd_touch_set_mirror_x(tp, false);
#endif
        break;
    case LV_DISP_ROT_90:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, false);
        esp_lcd_panel_mirror(panel_handle, true, true);
#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
        // Rotate LCD touch
        esp_lcd_touch_set_mirror_y(tp, true);
        esp_lcd_touch_set_mirror_x(tp, false);
#endif
        break;
    case LV_DISP_ROT_180:
        // Rotate LCD display
        esp_lcd_panel_swap_xy(panel_handle, true);
        esp_lcd_panel_mirror(panel_handle, true, false);
#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
        // Rotate LCD touch
        esp_lcd_touch_set_mirror_y(tp, true);
        esp_lcd_touch_set_mirror_x(tp, false);
#endif
        break;
    }
}

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
static void example_lvgl_touch_cb(lv_indev_drv_t * drv, lv_indev_data_t * data)
{
    // Use new API (v2.0.0) - esp_lcd_touch_get_data
    esp_lcd_touch_point_data_t touch_point;
    uint8_t point_cnt = 0;

    esp_lcd_touch_read_data(drv->user_data);
    esp_lcd_touch_get_data(drv->user_data, &touch_point, &point_cnt, 1);

    if (point_cnt > 0) {
        data->point.x = touch_point.x;
        data->point.y = touch_point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

void app_main(void)
{
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

    ESP_LOGI(TAG, "Turn off LCD backlight");
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_BK_LIGHT
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    ESP_LOGI(TAG, "Initialize SPI bus");
    spi_bus_config_t buscfg = {
        .sclk_io_num = EXAMPLE_PIN_NUM_SCLK,
        .mosi_io_num = EXAMPLE_PIN_NUM_MOSI,
        .miso_io_num = EXAMPLE_PIN_NUM_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * sizeof(uint16_t),  // 支持全屏传输
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_PIN_NUM_LCD_DC,
        .cs_gpio_num = EXAMPLE_PIN_NUM_LCD_CS,
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,
        .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 20,  // 增加传输队列深度
        .on_color_trans_done = example_notify_lvgl_flush_ready,
        .user_ctx = &disp_drv,
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
#if CONFIG_EXAMPLE_LCD_CONTROLLER_ILI9341
    ESP_LOGI(TAG, "Install ILI9341 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_config, &panel_handle));
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
    ESP_LOGI(TAG, "Install GC9A01 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
#elif CONFIG_EXAMPLE_LCD_CONTROLLER_ST7789
    ESP_LOGI(TAG, "Install ST7789 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
#endif
    s_panel_handle = panel_handle;  // 保存全局引用

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
#if CONFIG_EXAMPLE_LCD_CONTROLLER_GC9A01
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));  // 旋转180度
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    // user can flush pre-defined pattern to the screen before we turn on the screen or backlight
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    (void)tp_io_config;  // 标记为已使用，消除警告

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = 240,
        .y_max = 320,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 1,  // 触摸X轴镜像
            .mirror_y = 1,  // 触摸Y轴镜像
        },
    };

#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_STMPE610
    ESP_LOGI(TAG, "Initialize touch controller STMPE610");
    ESP_ERROR_CHECK(esp_lcd_touch_new_spi_stmpe610(tp_io_handle, &tp_cfg, &tp));
#endif // CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_STMPE610
#if CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_FT6336
    ESP_LOGI(TAG, "Initialize touch controller FT6336");
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_ft5x06(tp_io_handle, &tp_cfg, &tp));
#endif // CONFIG_EXAMPLE_LCD_TOUCH_CONTROLLER_FT6336
#endif // CONFIG_EXAMPLE_LCD_TOUCH_ENABLED

    ESP_LOGI(TAG, "Turn on LCD backlight");
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);

    // Initialize WiFi before LVGL (to avoid memory conflicts)
    ESP_LOGI(TAG, "Initializing WiFi...");
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    // Register WiFi event handler
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "WiFi initialized successfully");

    // Try to auto-connect to saved WiFi
    app_main_auto_connect_wifi();

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    // alloc draw buffers used by LVGL
    // 单缓冲40行：视频直接写LCD绕过LVGL，只需渲染UI控件
    // 240*40*2 = 19.2KB，为stream任务栈预留内存
    lv_color_t *buf1 = heap_caps_malloc(EXAMPLE_LCD_H_RES * 40 * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    // initialize LVGL draw buffers (单缓冲)
    lv_disp_draw_buf_init(&disp_buf, buf1, NULL, EXAMPLE_LCD_H_RES * 40);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.drv_update_cb = example_lvgl_port_update_callback;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    // Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

#if CONFIG_EXAMPLE_LCD_TOUCH_ENABLED
    static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    indev_drv.user_data = tp;

    lv_indev_drv_register(&indev_drv);
#endif

    ESP_LOGI(TAG, "Display LVGL Home Screen");
    ui_home_create();
    //setup_ui(&guider_ui);
    //example_lvgl_demo_ui(disp);
    //lv_demo_benchmark();
    //lv_demo_keypad_encoder();
    //lv_demo_stress();
    // lv_demo_widgets();
    //lv_demo_music();

    // WiFi initialization causes crash - temporarily disabled
    // ESP_LOGI(TAG, "Initializing WiFi");
    // ESP_ERROR_CHECK(esp_netif_init());
    // ESP_ERROR_CHECK(esp_event_loop_create_default());
    // esp_netif_create_default_wifi_sta();
    // wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    // ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    // ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // ESP_ERROR_CHECK(esp_wifi_start());
    // ESP_LOGI(TAG, "WiFi initialized successfully");

    while (1) {
        // 1ms延迟以获得最高刷新率
        vTaskDelay(pdMS_TO_TICKS(1));
        // The task running lv_timer_handler should have lower priority than that running `lv_tick_inc`
        lv_timer_handler();
    }
}
