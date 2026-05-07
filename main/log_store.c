#include "log_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define LOG_STORE_RING_SIZE 2048
#define LOG_STORE_LINE_SIZE 256

static char log_ring[LOG_STORE_RING_SIZE];
static size_t log_head = 0;
static size_t log_used = 0;
static bool log_store_ready = false;
static volatile bool log_store_enabled = false;
static vprintf_like_t original_vprintf = NULL;
static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;

static void log_store_append_locked(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        log_ring[log_head] = text[i];
        log_head = (log_head + 1) % LOG_STORE_RING_SIZE;
        if (log_used < LOG_STORE_RING_SIZE) {
            log_used++;
        }
    }
}

static int log_store_vprintf(const char *fmt, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    int ret;
    if (original_vprintf) {
        ret = original_vprintf(fmt, args);
    } else {
        ret = vprintf(fmt, args);
    }

    char line[LOG_STORE_LINE_SIZE];
    int len = vsnprintf(line, sizeof(line), fmt, args_copy);
    va_end(args_copy);

    if (log_store_enabled && len > 0) {
        size_t copy_len = (len < (int)sizeof(line)) ? (size_t)len : sizeof(line) - 1;
        portENTER_CRITICAL(&log_mux);
        log_store_append_locked(line, copy_len);
        portEXIT_CRITICAL(&log_mux);
    }

    return ret;
}

void log_store_init(void)
{
    if (log_store_ready) return;

    portENTER_CRITICAL(&log_mux);
    memset(log_ring, 0, sizeof(log_ring));
    log_head = 0;
    log_used = 0;
    log_store_ready = true;
    portEXIT_CRITICAL(&log_mux);

    original_vprintf = esp_log_set_vprintf(log_store_vprintf);
}

void log_store_enable(bool enabled)
{
    portENTER_CRITICAL(&log_mux);
    log_store_enabled = enabled;
    portEXIT_CRITICAL(&log_mux);
}

bool log_store_is_enabled(void)
{
    return log_store_enabled;
}

void log_store_clear(void)
{
    portENTER_CRITICAL(&log_mux);
    memset(log_ring, 0, sizeof(log_ring));
    log_head = 0;
    log_used = 0;
    portEXIT_CRITICAL(&log_mux);
}

size_t log_store_snapshot(char *out, size_t out_len)
{
    if (!out || out_len == 0) return 0;

    portENTER_CRITICAL(&log_mux);
    size_t used = log_used;
    size_t to_copy = used;
    if (to_copy > out_len - 1) {
        to_copy = out_len - 1;
    }
    size_t start = (log_head + LOG_STORE_RING_SIZE - to_copy) % LOG_STORE_RING_SIZE;
    for (size_t i = 0; i < to_copy; i++) {
        out[i] = log_ring[(start + i) % LOG_STORE_RING_SIZE];
    }
    portEXIT_CRITICAL(&log_mux);

    out[to_copy] = '\0';
    return to_copy;
}
