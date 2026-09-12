/**
 * @file    esp_at_config_default.h
 * @brief   ESP-AT 客户端库默认配置（用户不要改这里）
 */

#ifndef ESP_AT_CONFIG_DEFAULT_H
#define ESP_AT_CONFIG_DEFAULT_H

// 功能总开关。
#ifndef ESP_AT_ENABLE
#define ESP_AT_ENABLE            1
#endif

// 0=off 1=err 2=warn 3=info 4=debug 5=verbose（每条 AT 命令 / 每行 URC）
#ifndef ESP_AT_LOG_LEVEL
#define ESP_AT_LOG_LEVEL         3
#endif

// at_comms 标签的单独提级开关：1=启动时将 at_comms 提升到 VERBOSE（查看 << / >>）。
// 0=显式关闭。未定义时不改变 at_comms（全部使用全局默认）。
#ifndef ESP_AT_COMMS_VERBOSE_LOG
#define ESP_AT_COMMS_VERBOSE_LOG 0
#endif

#ifndef ESP_AT_UART_RX_BUF_SZ
#define ESP_AT_UART_RX_BUF_SZ    512         // HAL_UARTEx_ReceiveToIdle_DMA 单段字节数。
#endif

#ifndef ESP_AT_RINGBUFFER_SZ
#define ESP_AT_RINGBUFFER_SZ     2048        // 必须 2 的幂
#endif

// 原始 TCP +IPD 单帧缓存；应覆盖 HTTP 头和一个 OTA 数据分片。
#ifndef ESP_AT_IPD_BUF_SZ
#define ESP_AT_IPD_BUF_SZ       4096
#endif

#ifndef ESP_AT_LINE_MAX
#define ESP_AT_LINE_MAX          256         // 行缓冲上限
#endif

#ifndef ESP_AT_CMD_MAX
#define ESP_AT_CMD_MAX           384         // 单条 AT 命令上限（含 MQTT 参数）。
#endif

#ifndef ESP_AT_CMD_TIMEOUT_DEFAULT_MS
#define ESP_AT_CMD_TIMEOUT_DEFAULT_MS  3000
#endif

#ifndef ESP_AT_CMD_RETRY_MAX
#define ESP_AT_CMD_RETRY_MAX     2           // 同步命令忙重试次数
#endif

#ifndef ESP_AT_BOOT_TIMEOUT_MS
#define ESP_AT_BOOT_TIMEOUT_MS   8000        // ESP-AT ready URC 等待时长。
#endif

#ifndef ESP_AT_BOOT_RETRY_MAX
#define ESP_AT_BOOT_RETRY_MAX    3
#endif

#ifndef ESP_AT_TASK_RX_STACK
#define ESP_AT_TASK_RX_STACK     1024        // StackType_t 单位：1024 字 = 4 KiB。
#endif

#ifndef ESP_AT_TASK_TX_STACK
#define ESP_AT_TASK_TX_STACK     256         // 仅等待任务通知：256 字 = 1 KiB。
#endif

#ifndef ESP_AT_TASK_EVT_STACK
#define ESP_AT_TASK_EVT_STACK    1024        // OTA 回调在此执行：1024 字 = 4 KiB。
#endif

// 可由项目根目录的 main/esp_at_config_user.h 覆盖。
#if defined(__has_include)
#  if __has_include("esp_at_config_user.h")
#    include "esp_at_config_user.h"
#  endif
#endif

#endif /* ESP_AT_CONFIG_DEFAULT_H */
