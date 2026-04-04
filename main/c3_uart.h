/**
 * C3 UART通信模块 - 与K230通信
 *
 * 硬件连接:
 *   C3 GPIO18(TX) <-> K230 GPIO12(RXD)
 *   C3 GPIO19(RX) <-> K230 GPIO11(TXD)
 *   GND <-> GND
 */

#ifndef C3_UART_H
#define C3_UART_H

#include <stdbool.h>
#include <stdint.h>

// UART引脚定义 (根据C3板子GH1.25-5P座子丝印)
// 丝印: 5V | G | 18 | 19 | 3V3
// 颜色: 红 | 黑 | 黄 | 绿 | 粉
#define C3_UART_TX_PIN  18  // GPIO18 = TX (黄色线)
#define C3_UART_RX_PIN  19  // GPIO19 = RX (绿色线)
#define C3_UART_BAUD    115200
#define C3_UART_BUF_SIZE 256

// K230命令定义
#define CMD_XIAOZHI     "XIAOZHI"
#define CMD_BEEP_SHORT  "BEEP:SHORT"
#define CMD_BEEP_LONG   "BEEP:LONG"

/**
 * 初始化UART1 (与K230通信)
 * @return true=成功, false=失败
 */
bool c3_uart_init(void);

/**
 * 发送字符串到K230
 * @param data 要发送的字符串 (不含\r\n)
 * @return 发送的字节数, -1表示失败
 */
int c3_uart_send(const char *data);

/**
 * 发送XIAOZHI命令到K230 (触发蜂鸣器)
 * @return true=成功, false=失败
 */
bool c3_uart_send_xiaozhi(void);

/**
 * 读取K230响应 (非阻塞)
 * @param buf 接收缓冲区
 * @param len 缓冲区长度
 * @param timeout_ms 超时时间(毫秒)
 * @return 接收到的字节数, 0=无数据, -1=错误
 */
int c3_uart_read(char *buf, int len, int timeout_ms);

/**
 * 检查是否有数据可读
 * @return true=有数据, false=无数据
 */
bool c3_uart_has_data(void);

/**
 * 发送XIAOZHI命令到K230 (自动初始化)
 * 首次调用会自动初始化UART
 * @return true=成功, false=失败
 */
bool c3_uart_send_xiaozhi(void);

#endif // C3_UART_H
