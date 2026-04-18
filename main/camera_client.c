/**
 * K230视频流客户端实现
 * 支持MJPEG视频流接收和HTTP轮询获取进度
 */

#include "camera_client.h"
#include "jpeg_decoder.h"
#include "ui_camera.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "arpa/inet.h"

static const char *TAG = "k230_client";

// 自定义memmem实现（ESP32可能没有）
static void *my_memmem(const void *haystack, size_t haystacklen, const void *needle, size_t needlelen)
{
    if (needlelen == 0 || haystacklen < needlelen) {
        return NULL;
    }
    const uint8_t *h = (const uint8_t *)haystack;
    const uint8_t *n = (const uint8_t *)needle;

    for (size_t i = 0; i <= haystacklen - needlelen; i++) {
        if (h[i] == n[0] && memcmp(h + i, n, needlelen) == 0) {
            return (void *)(h + i);
        }
    }
    return NULL;
}

// 全局变量
static k230_client_config_t g_config;
static progress_callback_t g_progress_callback = NULL;
static frame_callback_t g_frame_callback = NULL;
static bool g_is_connected = false;
static bool g_stream_running = false;

static TaskHandle_t g_stream_task_handle = NULL;
static int g_stream_socket = -1;  // 用于强制关闭socket

// MJPEG流配置 - video buffer占153.6KB后剩余内存有限
// 流缓冲区仅用于接收数据，大帧会被丢弃跳过
#define MAX_JPEG_SIZE (50 * 1024)   // 最大JPEG帧大小 50KB
#define STREAM_BUFFER_SIZE (32 * 1024)  // 流缓冲区 32KB（静态分配）

// 静态分配流缓冲区，避免内存碎片导致malloc失败
static uint8_t stream_buffer[STREAM_BUFFER_SIZE];  // 32KB

// ============== HTTP轮询获取进度 ==============

/**
 * @brief 通过HTTP获取K230状态
 */
static esp_err_t http_get_status(int *progress, char *message, size_t msg_len, char *stage, size_t stage_len)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/status", g_config.host, g_config.http_port);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,  // 增加到10秒
        .buffer_size = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    // 读取响应数据
    int data_len = esp_http_client_get_content_length(client);
    if (data_len > 0 && data_len < 2048) {
        char *data = malloc(data_len + 1);
        if (data) {
            int read_len = esp_http_client_read(client, data, data_len);
            data[read_len] = '\0';

            ESP_LOGD(TAG, "Status response: %s", data);

            // 简单的JSON解析（不依赖cJSON）
            // 解析格式: {"progress": 45, "message": "xxx", "stage": "xxx"}
            char *p = data;

            // 查找 progress
            char *prog_str = strstr(p, "\"progress\"");
            if (prog_str && progress) {
                prog_str = strstr(prog_str, ":");
                if (prog_str) {
                    *progress = atoi(prog_str + 1);
                }
            }

            // 查找 message
            char *msg_str = strstr(p, "\"message\"");
            if (msg_str && message && msg_len > 0) {
                char *start = strchr(msg_str, '"');
                if (start) start = strchr(start + 1, '"');
                if (start) {
                    start++;
                    char *end = strchr(start, '"');
                    if (end) {
                        int len = end - start;
                        if (len < msg_len) {
                            strncpy(message, start, len);
                            message[len] = '\0';
                        }
                    }
                }
            }

            // 查找 stage
            char *stage_str = strstr(p, "\"stage\"");
            if (stage_str && stage && stage_len > 0) {
                char *start = strchr(stage_str, '"');
                if (start) start = strchr(start + 1, '"');
                if (start) {
                    start++;
                    char *end = strchr(start, '"');
                    if (end) {
                        int len = end - start;
                        if (len < stage_len) {
                            strncpy(stage, start, len);
                            stage[len] = '\0';
                        }
                    }
                }
            }

            free(data);
        }
    }

    esp_http_client_cleanup(client);
    return ESP_OK;
}

/**
 * @brief HTTP POST触发开始扫描
 */
static esp_err_t http_start_scan(void)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/api/start", g_config.host, g_config.http_port);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Start scan command sent");
    } else {
        ESP_LOGE(TAG, "Start scan failed: %s", esp_err_to_name(err));
    }

    return err;
}

// ============== MJPEG视频流接收 ==============

/**
 * @brief MJPEG流接收任务 - 使用原始TCP套接字
 *        esp_http_client不支持MJPEG流式传输（没有Content-Length）
 *        所以改用原始TCP socket直接发送HTTP请求并解析响应
 */
static void mjpeg_stream_task(void *arg)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overread"
    ESP_LOGI(TAG, "MJPEG stream task started (raw TCP)");

    // 使用静态分配的流缓冲区(32KB)

    // 创建TCP socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: errno=%d", errno);
        g_stream_running = false;
        vTaskDelete(NULL);
        return;
    }


    g_stream_socket = sock;  // 保存socket fd用于强制关闭

    // 设置接收超时20秒，发送超时10秒 (适应慢速网络如手机热点)
    // 注意: 手机热点可能非常慢，完整帧可能需要15秒以上才能到达
    struct timeval recv_timeout = { .tv_sec = 20, .tv_usec = 0 };
    struct timeval send_timeout = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

    // 禁用Nagle算法，减少网络延迟
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // 解析服务器地址
    struct sockaddr_in dest_addr = {0};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(g_config.http_port);
    if (inet_pton(AF_INET, g_config.host, &dest_addr.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid IP address: %s", g_config.host);
        close(sock);
        g_stream_socket = -1;
        g_stream_running = false;
        vTaskDelete(NULL);
        return;
    }

    // 连接服务器
    ESP_LOGI(TAG, "Connecting to %s:%d ...", g_config.host, g_config.http_port);
    int ret = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to connect: errno=%d", errno);
        close(sock);
        g_stream_socket = -1;
        g_stream_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TCP connected to K230");

    // 发送HTTP GET请求
    char request[256];
    snprintf(request, sizeof(request),
             "GET /api/stream HTTP/1.1\r\n"
             "Host: %s:%d\r\n"
             "Accept: multipart/x-mixed-replace\r\n"
             "Connection: close\r\n"
             "\r\n",
             g_config.host, g_config.http_port);

    int sent = send(sock, request, strlen(request), 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send request: errno=%d", errno);
        close(sock);
        g_stream_running = false;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "HTTP request sent (%d bytes)", sent);

    // 初始化JPEG解码器
    jpeg_decoder_init();

    // 接收数据：先跳过HTTP响应头
    size_t buffer_pos = 0;
    bool headers_parsed = false;
    size_t header_end_pos = 0;
    int frame_count = 0;
    int recv_count = 0;
    int recv_timeout_count = 0;
    int64_t last_frame_time = esp_timer_get_time();

    ESP_LOGI(TAG, "Starting recv loop...");

    while (g_stream_running && g_is_connected) {
        // 读取数据
        int read_len = recv(sock,
                             stream_buffer + buffer_pos,
                             STREAM_BUFFER_SIZE - buffer_pos,
                             0);

        if (read_len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                recv_timeout_count++;
                if (recv_timeout_count <= 3) {
                    ESP_LOGW(TAG, "Recv timeout #%d (no data for 20s)", recv_timeout_count);
                }
                continue;  // 超时，继续等待
            }
            ESP_LOGE(TAG, "Recv error: errno=%d", errno);
            break;
        }

        recv_count++;
        if (recv_count <= 3) {
            ESP_LOGI(TAG, "Recv #%d: got %d bytes (total buffer: %d)", recv_count, read_len, buffer_pos + read_len);
        }

        // 收到数据 → 通知UI层连接仍然存活
        ui_camera_heartbeat();

        if (read_len == 0) {
            ESP_LOGW(TAG, "Connection closed by server after %d recv calls", recv_count);
            break;
        }

        buffer_pos += read_len;

        // 第一次接收：跳过HTTP响应头
        if (!headers_parsed) {
            // 查找 \r\n\r\n (HTTP头结束)
            uint8_t *header_end = my_memmem(stream_buffer, buffer_pos, "\r\n\r\n", 4);
            if (!header_end) {
                if (buffer_pos >= 1024) {
                    ESP_LOGE(TAG, "HTTP headers too large, aborting");
                    break;
                }
                continue;  // 等待更多数据
            }

            // 解析HTTP状态码
            header_end_pos = (header_end - stream_buffer) + 4;
            int http_status = 0;
            if (buffer_pos > 12) {
                http_status = atoi((char *)stream_buffer + 9);  // "HTTP/1.1 200"
            }
            ESP_LOGI(TAG, "HTTP response status: %d, headers: %d bytes", http_status, header_end_pos);

            if (http_status != 200) {
                ESP_LOGE(TAG, "HTTP request failed with status %d", http_status);
                break;
            }

            // 打印Content-Type头以确认是MJPEG
            uint8_t *ct = my_memmem(stream_buffer, header_end_pos, "Content-Type:", 13);
            if (ct) {
                ct += 13;
                while (*ct == ' ') ct++;
                uint8_t *ct_end = my_memmem(ct, header_end_pos - (ct - stream_buffer), "\r\n", 2);
                if (ct_end) {
                    ESP_LOGI(TAG, "Content-Type: %.*s", (int)(ct_end - ct), ct);
                }
            }

            // 移除HTTP头，只保留body数据
            size_t body_len = buffer_pos - header_end_pos;
            memmove(stream_buffer, stream_buffer + header_end_pos, body_len);
            buffer_pos = body_len;
            headers_parsed = true;

            ESP_LOGI(TAG, "Headers parsed, body starts with %d bytes", body_len);
            // 打印前几个字节看是否是JPEG
            if (body_len > 0) {
                ESP_LOGI(TAG, "First bytes: %02X %02X %02X %02X",
                         stream_buffer[0], stream_buffer[1], stream_buffer[2], stream_buffer[3]);
            }
        }

        if (!headers_parsed) continue;

        // 查找JPEG帧（SOI=FFD8, EOI=FFD9）
        while (true) {
            size_t frame_size = 0;

            // 查找JPEG SOI标记
            const uint8_t *soi = my_memmem(stream_buffer, buffer_pos, "\xFF\xD8", 2);
            if (!soi) {
                // 没有SOI标记 - 数据可能不是JPEG
                if (recv_count <= 5) {
                    ESP_LOGW(TAG, "No SOI found in %d bytes. Data: %02X %02X %02X %02X %02X %02X %02X %02X",
                             buffer_pos,
                             stream_buffer[0], stream_buffer[1], stream_buffer[2], stream_buffer[3],
                             stream_buffer[4], stream_buffer[5], stream_buffer[6], stream_buffer[7]);
                }
                buffer_pos = 0;
                break;
            }

            // 跳过SOI之前的数据
            size_t skip = soi - stream_buffer;
            if (skip > 0) {
                memmove(stream_buffer, soi, buffer_pos - skip);
                buffer_pos -= skip;
            }

            // 查找JPEG EOI标记
            const uint8_t *eoi = my_memmem(stream_buffer + 2, buffer_pos - 2, "\xFF\xD9", 2);
            if (!eoi) {
                // 有SOI但没有EOI，等待更多数据
                if (buffer_pos >= STREAM_BUFFER_SIZE - 100) {
                    // 缓冲区快满了但没有完整帧，丢弃并重新同步
                    ESP_LOGW(TAG, "Buffer full, searching for next frame...");
                    const uint8_t *next_soi = my_memmem(stream_buffer + 2, buffer_pos - 2, "\xFF\xD8", 2);
                    if (next_soi) {
                        size_t skip = next_soi - stream_buffer;
                        memmove(stream_buffer, next_soi, buffer_pos - skip);
                        buffer_pos -= skip;
                        ESP_LOGW(TAG, "Resynced, skipped %d bytes", skip);
                    } else {
                        buffer_pos = 0;
                    }
                }
                break;
            }

            // 找到完整JPEG帧
            frame_size = (eoi - stream_buffer) + 2;

            // ===== 跳帧优化：检查缓冲区是否有更新的帧 =====
            // 如果EOI之后还有SOI，说明有更新的帧，跳过当前帧
            size_t remaining = buffer_pos - frame_size;
            if (remaining > 10) {
                const uint8_t *next_soi = my_memmem(stream_buffer + frame_size, remaining, "\xFF\xD8", 2);
                if (next_soi) {
                    // 有更新的帧，跳过当前帧
                    int skip_count = 1;
                    // 继续检查是否有更多帧可以跳过
                    while (true) {
                        size_t skip_offset = next_soi - stream_buffer;
                        const uint8_t *next_eoi = my_memmem(stream_buffer + skip_offset + 2, buffer_pos - skip_offset - 2, "\xFF\xD9", 2);
                        if (next_eoi) {
                            size_t next_frame_size = (next_eoi - next_soi) + 2;
                            if (next_frame_size >= 100 && next_frame_size <= MAX_JPEG_SIZE) {
                                // 检查是否还有更新的帧
                                size_t next_remaining = buffer_pos - (skip_offset + next_frame_size);
                                if (next_remaining > 10) {
                                    const uint8_t *newer_soi = my_memmem(stream_buffer + skip_offset + next_frame_size, next_remaining, "\xFF\xD8", 2);
                                    if (newer_soi) {
                                        // 还有更新的帧，继续跳过
                                        skip_count++;
                                        next_soi = newer_soi;
                                        continue;
                                    }
                                }
                            }
                        }
                        break;
                    }

                    // 移动最新帧到缓冲区开头
                    size_t skip_offset = next_soi - stream_buffer;
                    if (skip_count > 0) {
                        memmove(stream_buffer, next_soi, buffer_pos - skip_offset);
                        buffer_pos -= skip_offset;
                        // 重新查找EOI
                        eoi = my_memmem(stream_buffer + 2, buffer_pos - 2, "\xFF\xD9", 2);
                        if (!eoi) break;
                        frame_size = (eoi - stream_buffer) + 2;
                    }
                }
            }
            // ===== 跳帧优化结束 =====

            if (frame_count < 3) {
                ESP_LOGI(TAG, "Frame #%d: size=%d bytes, starts with %02X %02X",
                         frame_count + 1, frame_size, stream_buffer[0], stream_buffer[1]);
            }

            if (frame_size > MAX_JPEG_SIZE) {
                ESP_LOGW(TAG, "Frame too large: %d, skipping", frame_size);
                size_t next_pos = (eoi - stream_buffer) + 2;
                if (next_pos < buffer_pos) {
                    memmove(stream_buffer, stream_buffer + next_pos, buffer_pos - next_pos);
                    buffer_pos -= next_pos;
                } else {
                    buffer_pos = 0;
                }
                continue;
            }

            if (frame_size < 100) {
                ESP_LOGW(TAG, "Frame too small: %d, skipping", frame_size);
                size_t next_pos = (eoi - stream_buffer) + 2;
                if (next_pos < buffer_pos) {
                    memmove(stream_buffer, stream_buffer + next_pos, buffer_pos - next_pos);
                    buffer_pos -= next_pos;
                } else {
                    buffer_pos = 0;
                }
                continue;
            }

            // 回调传递原始JPEG数据
            if (g_frame_callback) {
                if (frame_count < 3) {
                    ESP_LOGI(TAG, "Calling frame_callback with %d bytes", frame_size);
                }
                g_frame_callback(stream_buffer, frame_size);
            } else {
                // frame_callback 为 NULL = 后台保活模式，视频流保持但不更新UI
                // 仅每100帧打印一次，避免日志过多
                if (frame_count % 100 == 0) {
                    ESP_LOGI(TAG, "Background mode: frame #%d received (%d bytes)", frame_count, frame_size);
                }
            }

            frame_count++;
            int64_t current_time = esp_timer_get_time();
            float fps = 1000000.0f / (current_time - last_frame_time);
            last_frame_time = current_time;

            if (frame_count % 30 == 0) {
                ESP_LOGI(TAG, "Received %d frames, fps: %.1f, last size: %d",
                         frame_count, fps, frame_size);
            }

            // 移除已处理的数据
            size_t next_pos = (eoi - stream_buffer) + 2;
            if (next_pos < buffer_pos) {
                memmove(stream_buffer, stream_buffer + next_pos, buffer_pos - next_pos);
                buffer_pos -= next_pos;
            } else {
                buffer_pos = 0;
            }
        }

        // 如果缓冲区使用量低，重置位置
        if (buffer_pos < 100) {
            buffer_pos = 0;
        }
    }

    // 清理
    if (g_stream_socket >= 0) {
        close(g_stream_socket);
        g_stream_socket = -1;
    }
    jpeg_decoder_deinit();

    ESP_LOGI(TAG, "MJPEG stream task ended (received %d frames total)", frame_count);
    g_stream_task_handle = NULL;
    g_stream_running = false;
    vTaskDelete(NULL);
#pragma GCC diagnostic pop
}

// ============== 公共API实现 ==============

esp_err_t k230_client_init(const k230_client_config_t *config, progress_callback_t callback)
{
    if (!config || config->host[0] == '\0') {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&g_config, config, sizeof(k230_client_config_t));
    g_progress_callback = callback;
    g_is_connected = false;
    g_stream_running = false;

    ESP_LOGI(TAG, "K230 client initialized: %s:%d", config->host, config->http_port);
    return ESP_OK;
}

esp_err_t k230_client_connect(void)
{
    // 测试连接
    int progress = 0;
    char message[128] = {0};
    char stage[32] = {0};

    ESP_LOGI(TAG, "Attempting to connect to K230 at %s:%d", g_config.host, g_config.http_port);

    esp_err_t err = http_get_status(&progress, message, sizeof(message), stage, sizeof(stage));

    if (err == ESP_OK) {
        g_is_connected = true;
        ESP_LOGI(TAG, "Connected to K230 successfully!");
        ESP_LOGI(TAG, "Status: progress=%d, stage=%s, message=%s", progress, stage, message);

        // 不在这里创建poll任务，避免和stream任务抢占K230的连接
        // poll任务只在需要时（扫描阶段）才启动

        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to K230: %s", esp_err_to_name(err));
    g_is_connected = false;
    return ESP_FAIL;
}

void k230_client_disconnect(void)
{
    g_is_connected = false;

    // 停止视频流
    k230_client_stop_stream();

    ESP_LOGI(TAG, "Disconnected from K230");
}

esp_err_t k230_client_start_scan(void)
{
    if (!g_is_connected) {
        ESP_LOGE(TAG, "Not connected to K230");
        return ESP_ERR_INVALID_STATE;
    }

    return http_start_scan();
}

bool k230_client_is_connected(void)
{
    return g_is_connected;
}

void k230_client_set_frame_callback(frame_callback_t callback)
{
    g_frame_callback = callback;
}

esp_err_t k230_client_start_stream(void)
{
    if (!g_is_connected) {
        ESP_LOGE(TAG, "Not connected to K230");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_stream_running) {
        ESP_LOGW(TAG, "Stream already running");
        return ESP_OK;
    }

    // 等待之前的任务完全清理 (防止内存碎片)
    vTaskDelay(pdMS_TO_TICKS(100));

    g_stream_running = true;
    g_stream_socket = -1;  // 重置socket fd

    // 打印内存状态
    ESP_LOGI(TAG, "Free heap before stream task: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // 创建MJPEG流接收任务 (带重试机制)
    // tjpgd解码器配置了JD_FASTDECODE=1, 栈需求较低
    BaseType_t ret = pdFAIL;
    uint16_t stack_sizes[] = {8192, 6144, 10240};  // 尝试8KB, 6KB, 10KB

    for (int attempt = 0; attempt < 3; attempt++) {
        ret = xTaskCreate(
            mjpeg_stream_task,
            "mjpeg_stream",
            stack_sizes[attempt],
            NULL,
            5,     // 优先级
            &g_stream_task_handle
        );

        if (ret == pdPASS) {
            ESP_LOGI(TAG, "Stream task created with stack=%d (attempt %d)", stack_sizes[attempt], attempt + 1);
            break;
        }

        ESP_LOGW(TAG, "Task create failed with stack=%d (attempt %d, heap=%lu)",
                 stack_sizes[attempt], attempt + 1, (unsigned long)esp_get_free_heap_size());

        // 等待内存释放后再试
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stream task after 3 attempts (free heap: %lu bytes)",
                 (unsigned long)esp_get_free_heap_size());
        g_stream_running = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "MJPEG stream started");
    return ESP_OK;
}

void k230_client_force_stop_stream(void)
{
    g_stream_running = false;
    g_is_connected = false;

    // 强制关闭socket让recv立即返回错误
    if (g_stream_socket >= 0) {
        ESP_LOGI(TAG, "Force closing stream socket");
        close(g_stream_socket);
        g_stream_socket = -1;
    }

    // 不等待任务退出，让它自行清理
    // recv()会因为socket关闭而返回错误，任务会自动退出
    ESP_LOGI(TAG, "Video stream stop requested (non-blocking)");
}

void k230_client_stop_stream(void)
{
    g_stream_running = false;
    g_is_connected = false;  // 让 recv() 立即返回
}


