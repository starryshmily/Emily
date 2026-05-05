/**
 * Model history storage implementation.
 */

#include "history_store.h"
#include "esp_log.h"
#include "nvs.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "history_store";
static const char *NVS_NAMESPACE = "model_history";
static const char *KEY_COUNT = "count";
static const char *KEY_NEXT = "next";

static bool initialized = false;

static void make_key(char *buf, size_t buf_len, const char *prefix, int index)
{
    snprintf(buf, buf_len, "%s%d", prefix, index);
}

esp_err_t history_store_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open NVS failed: %s", esp_err_to_name(err));
        return err;
    }

    int32_t count = 0;
    if (nvs_get_i32(handle, KEY_COUNT, &count) != ESP_OK) {
        nvs_set_i32(handle, KEY_COUNT, 0);
    }

    int32_t next = 0;
    if (nvs_get_i32(handle, KEY_NEXT, &next) != ESP_OK) {
        nvs_set_i32(handle, KEY_NEXT, 0);
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    initialized = (err == ESP_OK);
    return err;
}

esp_err_t history_store_add_full(const char *model_name,
                                 const char *created_time,
                                 const char *model_size,
                                 const char *pointcloud_size,
                                 const char *model_path)
{
    if (!model_name || model_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!created_time || created_time[0] == '\0') {
        created_time = "Unknown";
    }
    if (!model_size || model_size[0] == '\0') {
        model_size = "N/A";
    }
    if (!pointcloud_size || pointcloud_size[0] == '\0') {
        pointcloud_size = "N/A";
    }
    if (!model_path || model_path[0] == '\0') {
        model_path = "N/A";
    }

    esp_err_t err = history_store_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    int32_t count = 0;
    int32_t next = 0;
    nvs_get_i32(handle, KEY_COUNT, &count);
    nvs_get_i32(handle, KEY_NEXT, &next);

    if (next < 0 || next >= HISTORY_STORE_MAX_RECORDS) {
        next = 0;
    }

    char key_name[16];
    char key_time[16];
    char key_model_size[16];
    char key_point_size[16];
    char key_path[16];
    make_key(key_name, sizeof(key_name), "name", next);
    make_key(key_time, sizeof(key_time), "time", next);
    make_key(key_model_size, sizeof(key_model_size), "msize", next);
    make_key(key_point_size, sizeof(key_point_size), "psize", next);
    make_key(key_path, sizeof(key_path), "path", next);

    char safe_name[HISTORY_MODEL_NAME_LEN];
    char safe_time[HISTORY_CREATED_TIME_LEN];
    char safe_model_size[HISTORY_FILE_SIZE_LEN];
    char safe_point_size[HISTORY_FILE_SIZE_LEN];
    char safe_path[HISTORY_MODEL_PATH_LEN];
    snprintf(safe_name, sizeof(safe_name), "%s", model_name);
    snprintf(safe_time, sizeof(safe_time), "%s", created_time);
    snprintf(safe_model_size, sizeof(safe_model_size), "%s", model_size);
    snprintf(safe_point_size, sizeof(safe_point_size), "%s", pointcloud_size);
    snprintf(safe_path, sizeof(safe_path), "%s", model_path);

    err = nvs_set_str(handle, key_name, safe_name);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, key_time, safe_time);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, key_model_size, safe_model_size);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, key_point_size, safe_point_size);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, key_path, safe_path);
    }

    if (err == ESP_OK) {
        if (count < HISTORY_STORE_MAX_RECORDS) {
            count++;
        }
        next = (next + 1) % HISTORY_STORE_MAX_RECORDS;
        nvs_set_i32(handle, KEY_COUNT, count);
        nvs_set_i32(handle, KEY_NEXT, next);
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "added history: %s / %s / %s / %s", safe_name, safe_time, safe_model_size, safe_point_size);
    } else {
        ESP_LOGE(TAG, "add history failed: %s", esp_err_to_name(err));
    }

    return err;
}

esp_err_t history_store_add(const char *model_name, const char *created_time)
{
    return history_store_add_full(model_name, created_time, "N/A", "N/A", "N/A");
}

esp_err_t history_store_rename(int storage_index, const char *new_name)
{
    if (storage_index < 0 || storage_index >= HISTORY_STORE_MAX_RECORDS ||
        !new_name || new_name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = history_store_init();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char key_name[16];
    char safe_name[HISTORY_MODEL_NAME_LEN];
    make_key(key_name, sizeof(key_name), "name", storage_index);
    snprintf(safe_name, sizeof(safe_name), "%s", new_name);

    err = nvs_set_str(handle, key_name, safe_name);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "renamed history[%d]: %s", storage_index, safe_name);
    } else {
        ESP_LOGE(TAG, "rename history failed: %s", esp_err_to_name(err));
    }
    return err;
}

size_t history_store_count(void)
{
    if (history_store_init() != ESP_OK) {
        return 0;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }

    int32_t count = 0;
    nvs_get_i32(handle, KEY_COUNT, &count);
    nvs_close(handle);

    if (count < 0) {
        return 0;
    }
    if (count > HISTORY_STORE_MAX_RECORDS) {
        return HISTORY_STORE_MAX_RECORDS;
    }
    return (size_t)count;
}

size_t history_store_get_all(history_record_t *records, size_t max_records)
{
    if (!records || max_records == 0 || history_store_init() != ESP_OK) {
        return 0;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return 0;
    }

    int32_t count = 0;
    int32_t next = 0;
    nvs_get_i32(handle, KEY_COUNT, &count);
    nvs_get_i32(handle, KEY_NEXT, &next);

    if (count < 0) {
        count = 0;
    }
    if (count > HISTORY_STORE_MAX_RECORDS) {
        count = HISTORY_STORE_MAX_RECORDS;
    }
    if (next < 0 || next >= HISTORY_STORE_MAX_RECORDS) {
        next = 0;
    }

    size_t out_count = 0;
    for (int i = 0; i < count && out_count < max_records; i++) {
        int index = (next - 1 - i + HISTORY_STORE_MAX_RECORDS) % HISTORY_STORE_MAX_RECORDS;
        char key_name[16];
        char key_time[16];
        char key_model_size[16];
        char key_point_size[16];
        char key_path[16];
        make_key(key_name, sizeof(key_name), "name", index);
        make_key(key_time, sizeof(key_time), "time", index);
        make_key(key_model_size, sizeof(key_model_size), "msize", index);
        make_key(key_point_size, sizeof(key_point_size), "psize", index);
        make_key(key_path, sizeof(key_path), "path", index);

        size_t name_len = HISTORY_MODEL_NAME_LEN;
        size_t time_len = HISTORY_CREATED_TIME_LEN;
        size_t model_size_len = HISTORY_FILE_SIZE_LEN;
        size_t point_size_len = HISTORY_FILE_SIZE_LEN;
        size_t path_len = HISTORY_MODEL_PATH_LEN;
        esp_err_t name_err = nvs_get_str(handle, key_name, records[out_count].model_name, &name_len);
        esp_err_t time_err = nvs_get_str(handle, key_time, records[out_count].created_time, &time_len);
        esp_err_t model_size_err = nvs_get_str(handle, key_model_size, records[out_count].model_size, &model_size_len);
        esp_err_t point_size_err = nvs_get_str(handle, key_point_size, records[out_count].pointcloud_size, &point_size_len);
        esp_err_t path_err = nvs_get_str(handle, key_path, records[out_count].model_path, &path_len);

        if (name_err == ESP_OK) {
            if (time_err != ESP_OK) {
                snprintf(records[out_count].created_time, HISTORY_CREATED_TIME_LEN, "Unknown");
            }
            if (model_size_err != ESP_OK) {
                snprintf(records[out_count].model_size, HISTORY_FILE_SIZE_LEN, "N/A");
            }
            if (point_size_err != ESP_OK) {
                snprintf(records[out_count].pointcloud_size, HISTORY_FILE_SIZE_LEN, "N/A");
            }
            if (path_err != ESP_OK) {
                snprintf(records[out_count].model_path, HISTORY_MODEL_PATH_LEN, "N/A");
            }
            records[out_count].storage_index = index;
            out_count++;
        }
    }

    nvs_close(handle);
    return out_count;
}
