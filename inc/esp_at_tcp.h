/**
 * @file    esp_at_tcp.h
 * @brief   raw TCP + 自拼 HTTP/1.1 客户端（绕开 ESP-AT HTTPCLIENT state machine bug）
 */

#ifndef ESP_AT_TCP_H
#define ESP_AT_TCP_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_at_client.h"

#ifdef __cplusplus
extern "C" {
#endif

// TCP 连接上下文（caller 提供 buffer，避免 heap 分配）
typedef struct esp_at_tcp {
    char     host[64];                 // server 主机名或 IP
    uint16_t port;                     // server 端口
    uint8_t  link_id;                  // ESP-AT link_id（多连接时用）
    bool     connected;                // 当前连接状态
} esp_at_tcp_t;

/**
 * @brief 在其他网络连接建立前启用 AT+CIPMUX=1
 *
 * OTA 与 MQTT 并行使用时必须先调用；esp_at_tcp_connect() 也会兜底调用。
 */
esp_at_err_t esp_at_tcp_init(void);

/**
 * @brief 建 TCP 长连接（底层 AT+CIPSTART）
 */
esp_at_err_t esp_at_tcp_connect(esp_at_tcp_t *tcp,
                                 const char *host, uint16_t port,
                                 uint32_t timeout_ms);

/**
 * @brief 关连接（底层 AT+CIPCLOSE）
 */
esp_at_err_t esp_at_tcp_close(esp_at_tcp_t *tcp);

/**
 * @brief 自拼 HTTP/1.1 GET 请求（带 Range 头），走 AT+CIPSEND
 */
esp_at_err_t esp_at_tcp_http_get_range(esp_at_tcp_t *tcp,
                                       const char *host, const char *path,
                                       uint32_t offset, uint32_t len,
                                       uint32_t timeout_ms);

/**
 * @brief 收一段响应 body 到 out[]
 */
esp_at_err_t esp_at_tcp_recv_body(esp_at_tcp_t *tcp,
                                   uint8_t *out, uint32_t want,
                                   uint32_t *got, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ESP_AT_TCP_H */
