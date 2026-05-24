#include "developer_mode.h"
#include "log_store.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "developer_mode";
static const char *NVS_NS = "dev_mode";

static bool upload_test_enabled = false;
static bool history_test_enabled = false;
static bool uart_test_enabled = false;
static bool camera_test_enabled = false;
static bool motor_test_enabled = false;
static bool log_test_enabled = false;
static bool stl_ply_test_enabled = false;
static bool progress_bar_test_enabled = false;
static bool developer_mode_loaded = false;

static void developer_mode_save_u8(const char *key, bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs failed for %s: %s", key, esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(nvs, key, enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed: %s", key, esp_err_to_name(err));
    }
    nvs_close(nvs);
}

static bool developer_mode_load_u8(nvs_handle_t nvs, const char *key, bool default_value)
{
    uint8_t value = default_value ? 1 : 0;
    esp_err_t err = nvs_get_u8(nvs, key, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "load %s failed: %s", key, esp_err_to_name(err));
    }
    return value != 0;
}

esp_err_t developer_mode_init(void)
{
    if (developer_mode_loaded) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs failed: %s", esp_err_to_name(err));
        return err;
    }

    upload_test_enabled = developer_mode_load_u8(nvs, "upload", false);
    history_test_enabled = developer_mode_load_u8(nvs, "history", false);
    uart_test_enabled = developer_mode_load_u8(nvs, "uart", false);
    camera_test_enabled = developer_mode_load_u8(nvs, "camera", false);
    motor_test_enabled = developer_mode_load_u8(nvs, "motor", false);
    log_test_enabled = developer_mode_load_u8(nvs, "log", false);
    stl_ply_test_enabled = developer_mode_load_u8(nvs, "stl_ply", false);
    progress_bar_test_enabled = developer_mode_load_u8(nvs, "progress", false);
    log_store_enable(log_test_enabled);
    developer_mode_loaded = true;

    ESP_LOGI(TAG, "loaded: upload=%d history=%d uart=%d camera=%d motor=%d log=%d stl_ply=%d progress=%d",
             upload_test_enabled, history_test_enabled, uart_test_enabled,
             camera_test_enabled, motor_test_enabled, log_test_enabled,
             stl_ply_test_enabled, progress_bar_test_enabled);
    nvs_close(nvs);
    return ESP_OK;
}

void developer_mode_set_upload_test(bool enabled)
{
    developer_mode_init();
    upload_test_enabled = enabled;
    developer_mode_save_u8("upload", enabled);
}

bool developer_mode_is_upload_test(void)
{
    developer_mode_init();
    return upload_test_enabled;
}

void developer_mode_set_history_test(bool enabled)
{
    developer_mode_init();
    history_test_enabled = enabled;
    developer_mode_save_u8("history", enabled);
}

bool developer_mode_is_history_test(void)
{
    developer_mode_init();
    return history_test_enabled;
}

void developer_mode_set_uart_test(bool enabled)
{
    developer_mode_init();
    uart_test_enabled = enabled;
    developer_mode_save_u8("uart", enabled);
}

bool developer_mode_is_uart_test(void)
{
    developer_mode_init();
    return uart_test_enabled;
}

void developer_mode_set_camera_test(bool enabled)
{
    developer_mode_init();
    camera_test_enabled = enabled;
    developer_mode_save_u8("camera", enabled);
}

bool developer_mode_is_camera_test(void)
{
    developer_mode_init();
    return camera_test_enabled;
}

void developer_mode_set_motor_test(bool enabled)
{
    developer_mode_init();
    motor_test_enabled = enabled;
    developer_mode_save_u8("motor", enabled);
}

bool developer_mode_is_motor_test(void)
{
    developer_mode_init();
    return motor_test_enabled;
}

void developer_mode_set_ws2812_test(bool enabled)
{
    (void)enabled;
}

bool developer_mode_is_ws2812_test(void)
{
    return false;
}

void developer_mode_set_log_test(bool enabled)
{
    developer_mode_init();
    log_test_enabled = enabled;
    log_store_enable(enabled);
    developer_mode_save_u8("log", enabled);
}

bool developer_mode_is_log_test(void)
{
    developer_mode_init();
    return log_test_enabled;
}

void developer_mode_set_stl_ply_test(bool enabled)
{
    developer_mode_init();
    stl_ply_test_enabled = enabled;
    developer_mode_save_u8("stl_ply", enabled);
}

bool developer_mode_is_stl_ply_test(void)
{
    developer_mode_init();
    return stl_ply_test_enabled;
}

void developer_mode_set_progress_bar_test(bool enabled)
{
    developer_mode_init();
    progress_bar_test_enabled = enabled;
    developer_mode_save_u8("progress", enabled);
}

bool developer_mode_is_progress_bar_test(void)
{
    developer_mode_init();
    return progress_bar_test_enabled;
}

bool developer_mode_any_enabled(void)
{
    developer_mode_init();
    return upload_test_enabled || history_test_enabled || uart_test_enabled ||
           camera_test_enabled || motor_test_enabled || log_test_enabled ||
           stl_ply_test_enabled || progress_bar_test_enabled;
}
