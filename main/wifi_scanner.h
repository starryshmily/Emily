/**
 * WiFi Scanner - Simplified version for ESP32-C3
 * WiFi driver is initialized in main.c to avoid memory conflicts
 */

#ifndef WIFI_SCANNER_H
#define WIFI_SCANNER_H

#include <stdbool.h>
#include "esp_err.h"

// WiFi AP info (minimal structure to save memory)
typedef struct {
    char ssid[33];
    int8_t rssi;
} wifi_ap_info_t;

// Initialize WiFi scanner (call once at startup)
esp_err_t wifi_scanner_init(void);

// Start WiFi scan (blocking, ~2 seconds)
// Results are cached internally
esp_err_t wifi_scanner_start_scan(void);

// Get scan results (returns cached results from start_scan)
// Returns number of APs found (max 10)
int wifi_scanner_get_results(wifi_ap_info_t *ap_info, int max_count);

// Cleanup WiFi scanner (call when done)
void wifi_scanner_deinit(void);

#endif // WIFI_SCANNER_H
