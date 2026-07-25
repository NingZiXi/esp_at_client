/**
 * @file    esp_at_config_default.h
 * @brief   ESP-AT 客户端库默认配置（用户不要改这里）
 */

#ifndef ESP_AT_CONFIG_DEFAULT_H
#define ESP_AT_CONFIG_DEFAULT_H

// 总开关
#ifndef ESP_AT_ENABLE
#define ESP_AT_ENABLE            1
#endif

// 0=off 1=err 2=warn 3=info 4=debug
#ifndef ESP_AT_LOG_LEVEL
#define ESP_AT_LOG_LEVEL         3
#endif

// 关键路径调试日志（独立于 ESP_AT_LOG_LEVEL）
#ifndef ESP_AT_DEBUG_LOG
#define ESP_AT_DEBUG_LOG         0
#endif

#ifndef ESP_AT_UART_RX_BUF_SZ
#define ESP_AT_UART_RX_BUF_SZ    512         // HAL_UARTEx_ReceiveToIdle_DMA 单段字节数
#endif

#ifndef ESP_AT_RINGBUFFER_SZ
#define ESP_AT_RINGBUFFER_SZ     2048        // 必须 2 的幂
#endif

#ifndef ESP_AT_LINE_MAX
#define ESP_AT_LINE_MAX          256         // 行缓冲上限
#endif

#ifndef ESP_AT_CMD_TIMEOUT_DEFAULT_MS
#define ESP_AT_CMD_TIMEOUT_DEFAULT_MS  3000
#endif

#ifndef ESP_AT_CMD_RETRY_MAX
#define ESP_AT_CMD_RETRY_MAX     2           // 同步命令忙重试次数
#endif

#ifndef ESP_AT_BOOT_TIMEOUT_MS
#define ESP_AT_BOOT_TIMEOUT_MS   8000        // ESP-AT ready URC 等待
#endif

#ifndef ESP_AT_BOOT_RETRY_MAX
#define ESP_AT_BOOT_RETRY_MAX    3
#endif

#ifndef ESP_AT_TASK_RX_STACK
#define ESP_AT_TASK_RX_STACK     512
#endif

#ifndef ESP_AT_TASK_TX_STACK
#define ESP_AT_TASK_TX_STACK     384
#endif

#ifndef ESP_AT_TASK_EVT_STACK
#define ESP_AT_TASK_EVT_STACK    512
#endif

// 项目根 main/esp_at_config_user.h 可选覆盖
#if defined(__has_include)
#  if __has_include("esp_at_config_user.h")
#    include "esp_at_config_user.h"
#  endif
#endif

#endif /* ESP_AT_CONFIG_DEFAULT_H */