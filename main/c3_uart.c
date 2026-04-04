/**
 * C3 UART通信模块 - 与K230通信
 * GPIO18=TX, GPIO19=RX, 115200 8N1
 */

#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "c3_uart.h"

static const char *TAG = "C3_UART";

// UART端口号 (UART1, UART0通常用于USB串口)
#define C3_UART_NUM UART_NUM_1

// 是否已初始化
static bool uart_initialized = false;

bool c3_uart_init(void)
{
    if (uart_initialized) {
        ESP_LOGW(TAG, "UART already initialized, skipping");
        return true;
    }

    ESP_LOGI(TAG, "=== UART Init Start ===");
    ESP_LOGI(TAG, "Config: TX=GPIO%d, RX=GPIO%d, baud=%d, 8N1",
             C3_UART_TX_PIN, C3_UART_RX_PIN, C3_UART_BAUD);

    // UART配置
    uart_config_t uart_config = {
        .baud_rate = C3_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    // 安装UART驱动
    ESP_LOGI(TAG, "Installing UART driver (buf=%d)...", C3_UART_BUF_SIZE * 2);
    esp_err_t err = uart_driver_install(
        C3_UART_NUM,
        C3_UART_BUF_SIZE * 2,  // RX缓冲区
        C3_UART_BUF_SIZE * 2,  // TX缓冲区
        0,                      // 队列大小
        NULL,                   // 队列句柄
        0                       // 中断分配标志
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART driver install FAILED: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "UART driver install OK");

    // 配置UART参数
    err = uart_param_config(C3_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART param config FAILED: %s", esp_err_to_name(err));
        uart_driver_delete(C3_UART_NUM);
        return false;
    }
    ESP_LOGI(TAG, "UART param config OK");

    // 设置UART引脚
    err = uart_set_pin(C3_UART_NUM, C3_UART_TX_PIN, C3_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin FAILED: %s", esp_err_to_name(err));
        uart_driver_delete(C3_UART_NUM);
        return false;
    }
    ESP_LOGI(TAG, "UART pin set OK: GPIO%d=TX, GPIO%d=RX", C3_UART_TX_PIN, C3_UART_RX_PIN);

    uart_initialized = true;
    ESP_LOGI(TAG, "=== UART Init SUCCESS ===");
    return true;
}

int c3_uart_send(const char *data)
{
    if (!uart_initialized) {
        ESP_LOGE(TAG, "UART not initialized, cannot send!");
        return -1;
    }

    int len = strlen(data);
    ESP_LOGI(TAG, "=== Sending UART Data ===");
    ESP_LOGI(TAG, "  Data: [%s]", data);
    ESP_LOGI(TAG, "  Length: %d bytes", len);

    int sent = uart_write_bytes(C3_UART_NUM, data, len);

    if (sent < 0) {
        ESP_LOGE(TAG, "  ERROR: uart_write_bytes failed!");
        return -1;
    }
    ESP_LOGI(TAG, "  Sent %d bytes (data part)", sent);

    // 添加\r\n结尾
    int crlf_sent = uart_write_bytes(C3_UART_NUM, "\r\n", 2);
    if (crlf_sent < 0) {
        ESP_LOGE(TAG, "  ERROR: failed to send CRLF!");
        return -1;
    }
    sent += 2;
    ESP_LOGI(TAG, "  Sent CRLF (2 bytes)");
    ESP_LOGI(TAG, "  Total sent: %d bytes", sent);
    ESP_LOGI(TAG, "=== Send Complete ===");

    // 等待发送完成
    uart_wait_tx_done(C3_UART_NUM, pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "TX done (flushed to hardware)");

    return sent;
}

int c3_uart_read(char *buf, int len, int timeout_ms)
{
    if (!uart_initialized) {
        ESP_LOGW(TAG, "c3_uart_read: UART not initialized");
        return -1;
    }

    ESP_LOGD(TAG, "c3_uart_read: waiting %dms, buf size=%d", timeout_ms, len);
    int rx_len = uart_read_bytes(C3_UART_NUM, (uint8_t*)buf, len, pdMS_TO_TICKS(timeout_ms));

    if (rx_len > 0) {
        buf[rx_len] = '\0';  // 添加字符串结尾
        ESP_LOGI(TAG, "c3_uart_read: received %d bytes: [%s]", rx_len, buf);
    } else if (rx_len == 0) {
        ESP_LOGD(TAG, "c3_uart_read: timeout (no data)");
    } else {
        ESP_LOGW(TAG, "c3_uart_read: error (returned %d)", rx_len);
    }

    return rx_len;
}

bool c3_uart_has_data(void)
{
    if (!uart_initialized) {
        return false;
    }

    size_t buffered_size;
    uart_get_buffered_data_len(C3_UART_NUM, &buffered_size);
    return (buffered_size > 0);
}

bool c3_uart_send_xiaozhi(void)
{
    ESP_LOGI(TAG, ">>> c3_uart_send_xiaozhi() called <<<");

    // 自动初始化 (首次调用时)
    if (!uart_initialized) {
        ESP_LOGI(TAG, "First call - initializing UART...");
        if (!c3_uart_init()) {
            ESP_LOGE(TAG, "UART init failed! Cannot send XIAOZHI");
            return false;
        }
    }

    ESP_LOGI(TAG, "Sending XIAOZHI command to K230...");
    bool result = c3_uart_send(CMD_XIAOZHI) > 0;

    if (result) {
        ESP_LOGI(TAG, "XIAOZHI command sent successfully!");

        // 尝试读取K230响应
        ESP_LOGI(TAG, "Waiting for K230 response (500ms)...");
        char resp_buf[128] = {0};
        int resp_len = c3_uart_read(resp_buf, sizeof(resp_buf) - 1, 500);
        if (resp_len > 0) {
            ESP_LOGI(TAG, "K230 responded: [%s] (%d bytes)", resp_buf, resp_len);
        } else {
            ESP_LOGW(TAG, "No response from K230 (timeout or wiring issue?)");
            ESP_LOGW(TAG, "Check wiring:");
            ESP_LOGW(TAG, "  C3 黄色(GPIO18/TX) -> K230 白色(GPIO12/RXD)");
            ESP_LOGW(TAG, "  C3 绿色(GPIO19/RX) -> K230 绿色(GPIO11/TXD)");
            ESP_LOGW(TAG, "  C3 黑色(GND)     -> K230 黑色(GND)");
        }
    } else {
        ESP_LOGE(TAG, "XIAOZHI command send FAILED!");
    }

    return result;
}
