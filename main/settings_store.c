#include "settings_store.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings_store";
static const char *NVS_NS = "settings";
static const char *KEY_WS2812 = "ws2812";
static const char *KEY_LIGHT_COLOR = "light_color";
static const char *KEY_BOTTOM_CAM = "bottom_cam";

static bool settings_loaded = false;
static bool ws2812_enabled = true;
static uint8_t light_color = 135;
static bool bottom_cam_enabled = true;

static void settings_store_save_u8(const char *key, bool enabled)
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

esp_err_t settings_store_init(void)
{
    if (settings_loaded) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t value = 1;
    err = nvs_get_u8(nvs, KEY_WS2812, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "load ws2812 failed: %s", esp_err_to_name(err));
    }
    ws2812_enabled = value != 0;

    value = light_color;
    err = nvs_get_u8(nvs, KEY_LIGHT_COLOR, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "load light color failed: %s", esp_err_to_name(err));
    }
    light_color = value;

    value = 1;
    err = nvs_get_u8(nvs, KEY_BOTTOM_CAM, &value);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "load bottom_cam failed: %s", esp_err_to_name(err));
    }
    bottom_cam_enabled = value != 0;

    settings_loaded = true;
    ESP_LOGI(TAG, "loaded: ws2812=%d light_color=%d bottom_cam=%d", ws2812_enabled, light_color, bottom_cam_enabled);
    nvs_close(nvs);
    return ESP_OK;
}

void settings_store_set_ws2812_enabled(bool enabled)
{
    settings_store_init();
    ws2812_enabled = enabled;
    settings_store_save_u8(KEY_WS2812, enabled);
}

bool settings_store_is_ws2812_enabled(void)
{
    settings_store_init();
    return ws2812_enabled;
}

void settings_store_set_light_color(uint8_t hue)
{
    settings_store_init();
    light_color = hue;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open nvs failed for light color: %s", esp_err_to_name(err));
        return;
    }
    err = nvs_set_u8(nvs, KEY_LIGHT_COLOR, hue);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save light color failed: %s", esp_err_to_name(err));
    }
    nvs_close(nvs);
}

uint8_t settings_store_get_light_color(void)
{
    settings_store_init();
    return light_color;
}

void settings_store_set_bottom_cam_enabled(bool enabled)
{
    settings_store_init();
    bottom_cam_enabled = enabled;
    settings_store_save_u8(KEY_BOTTOM_CAM, enabled);
}

bool settings_store_is_bottom_cam_enabled(void)
{
    settings_store_init();
    return bottom_cam_enabled;
}
