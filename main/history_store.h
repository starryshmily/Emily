/**
 * Model history storage.
 *
 * Stores finished model name and creation time in NVS.
 */

#ifndef HISTORY_STORE_H
#define HISTORY_STORE_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HISTORY_STORE_MAX_RECORDS 20
#define HISTORY_MODEL_NAME_LEN 64
#define HISTORY_CREATED_TIME_LEN 32
#define HISTORY_FILE_SIZE_LEN 24
#define HISTORY_MODEL_PATH_LEN 96

typedef struct {
    char model_name[HISTORY_MODEL_NAME_LEN];
    char created_time[HISTORY_CREATED_TIME_LEN];
    char model_size[HISTORY_FILE_SIZE_LEN];
    char pointcloud_size[HISTORY_FILE_SIZE_LEN];
    char model_path[HISTORY_MODEL_PATH_LEN];
    int storage_index;
} history_record_t;

esp_err_t history_store_init(void);
esp_err_t history_store_add(const char *model_name, const char *created_time);
esp_err_t history_store_add_full(const char *model_name,
                                 const char *created_time,
                                 const char *model_size,
                                 const char *pointcloud_size,
                                 const char *model_path);
esp_err_t history_store_rename(int storage_index, const char *new_name);
size_t history_store_count(void);
size_t history_store_get_all(history_record_t *records, size_t max_records);

#ifdef __cplusplus
}
#endif

#endif // HISTORY_STORE_H
