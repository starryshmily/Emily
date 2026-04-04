/**
 * WiFi Scanner - Simplified version for ESP32-C3
 * WiFi driver is initialized in main.c to avoid memory conflicts
 */

#include "wifi_scanner.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "wifi_scanner";
static wifi_ap_info_t scan_results_buffer[30];
static int scan_result_count = 0;
static bool initialized = false;

// Simple comparison for sorting by RSSI (strongest first)
static int compare_rssi(const void *a, const void *b) {
    const wifi_ap_info_t *ap_a = (const wifi_ap_info_t *)a;
    const wifi_ap_info_t *ap_b = (const wifi_ap_info_t *)b;
    return ap_b->rssi - ap_a->rssi;
}

// Remove duplicate SSIDs (keep strongest signal)
static int remove_duplicates(wifi_ap_info_t *aps, int count) {
    if(count <= 1) return count;

    int unique_count = 0;
    for(int i = 0; i < count; i++) {
        bool is_duplicate = false;
        for(int j = 0; j < unique_count; j++) {
            if(strcmp(aps[i].ssid, aps[j].ssid) == 0) {
                is_duplicate = true;
                break;
            }
        }
        if(!is_duplicate) {
            if(unique_count != i) {
                aps[unique_count] = aps[i];
            }
            unique_count++;
        }
    }
    return unique_count;
}

// Initialize WiFi scanner (WiFi already initialized in main.c)
esp_err_t wifi_scanner_init(void)
{
    if(initialized) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "WiFi scanner init (driver already loaded in main)");
    memset(scan_results_buffer, 0, sizeof(scan_results_buffer));
    scan_result_count = 0;
    initialized = true;
    return ESP_OK;
}

// Start WiFi scan (blocking, but quick - ~2 seconds)
esp_err_t wifi_scanner_start_scan(void)
{
    if(!initialized) {
        ESP_LOGE(TAG, "WiFi scanner not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting WiFi scan...");
    scan_result_count = 0;

    // Start scan in blocking mode (returns when complete)
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if(ret != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Scan complete");

    // Immediately get results after scan completes
    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if(ret == ESP_OK && ap_count > 0) {
        ESP_LOGI(TAG, "Found %d APs", ap_count);

        // Limit to buffer size
        if(ap_count > 30) {
            ap_count = 30;
        }

        // Get AP records
        wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
        if(ap_records == NULL) {
            ESP_LOGE(TAG, "Failed to allocate AP records");
            return ESP_ERR_NO_MEM;
        }

        uint16_t number = ap_count;
        ret = esp_wifi_scan_get_ap_records(&number, ap_records);
        if(ret == ESP_OK) {
            // Copy to our buffer
            scan_result_count = (ap_count < 30) ? ap_count : 30;
            for(int i = 0; i < scan_result_count; i++) {
                strncpy(scan_results_buffer[i].ssid, (char *)ap_records[i].ssid, 32);
                scan_results_buffer[i].ssid[32] = '\0';
                scan_results_buffer[i].rssi = ap_records[i].rssi;
            }

            // Sort by signal strength (strongest first)
            qsort(scan_results_buffer, scan_result_count, sizeof(wifi_ap_info_t), compare_rssi);

            // Remove duplicates (same SSID)
            scan_result_count = remove_duplicates(scan_results_buffer, scan_result_count);

            ESP_LOGI(TAG, "After dedup: %d unique APs", scan_result_count);
        }
        free(ap_records);
    }

    return ESP_OK;
}

// Get scan results (return cached results from scan)
int wifi_scanner_get_results(wifi_ap_info_t *ap_info, int max_count)
{
    if(!initialized) {
        ESP_LOGE(TAG, "WiFi scanner not initialized");
        return 0;
    }

    if(ap_info == NULL || max_count <= 0) {
        ESP_LOGE(TAG, "Invalid parameters");
        return 0;
    }

    ESP_LOGI(TAG, "Returning %d cached scan results", scan_result_count);

    // Copy cached results to output (limit to max_count and 10)
    int copy_count = scan_result_count;
    if(copy_count > max_count) {
        copy_count = max_count;
    }
    if(copy_count > 10) {
        copy_count = 10;
    }

    for(int i = 0; i < copy_count; i++) {
        strncpy(ap_info[i].ssid, scan_results_buffer[i].ssid, 32);
        ap_info[i].ssid[32] = '\0';
        ap_info[i].rssi = scan_results_buffer[i].rssi;
    }

    return copy_count;
}

// Cleanup WiFi scanner
void wifi_scanner_deinit(void)
{
    if(!initialized) {
        return;
    }

    ESP_LOGI(TAG, "WiFi scanner deinit");
    memset(scan_results_buffer, 0, sizeof(scan_results_buffer));
    scan_result_count = 0;
    initialized = false;
}
