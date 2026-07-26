/**
 * @file    esp_at_types.h
 * @brief   ESP-AT 客户端公共类型定义
 */

#ifndef ESP_AT_TYPES_H
#define ESP_AT_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// 通用错误码
typedef enum {
    ESP_AT_OK              =  0,
    ESP_AT_ERR_FAIL        = -1,
    ESP_AT_ERR_TIMEOUT     = -2,
    ESP_AT_ERR_BUSY        = -3,
    ESP_AT_ERR_INVALID_ARG = -4,
    ESP_AT_ERR_NO_MEM      = -5,
    ESP_AT_ERR_NOT_READY   = -6,
    ESP_AT_ERR_NOT_SUPPORTED= -7,
    ESP_AT_ERR_PROTO       = -8,
    ESP_AT_ERR_RESP        = -9,
} esp_at_err_t;

// 单条 AT 命令响应状态
typedef enum {
    AT_RESP_OK            = 0,
    AT_RESP_ERROR         = 1,   // ERROR / +CWJAP:<errcode> ERROR
    AT_RESP_FAIL          = 2,
    AT_RESP_TIMEOUT       = 3,
    AT_RESP_BUSY          = 4,   // busy p...
    AT_RESP_ABORTED       = 5,
} at_resp_status_t;

#ifndef AT_RESP_TEXT_MAX
#define AT_RESP_TEXT_MAX         512
#endif

// 单条命令响应
typedef struct {
    at_resp_status_t status;
    uint32_t         err_code;                  // 解析自 "ERR CODE:0x%08x"，未取到为 0
    char             text[AT_RESP_TEXT_MAX];    // 命令直响应文本（不含 OK/ERROR 单独行）
    uint16_t         text_len;
    uint32_t         elapsed_ms;
} at_cmd_response_t;

// 事件枚举：ESP_AT_EVENT_ANY = -1 是通配（register-only）
typedef enum {
    ESP_AT_EVENT_ANY               = -1,   // 通配（register-only，dispatcher 对所有事件多派发一次）
    ESP_AT_EVENT_READY             = 0,
    ESP_AT_EVENT_WIFI_CONNECTED    = 1,
    ESP_AT_EVENT_WIFI_GOT_IP       = 2,
    ESP_AT_EVENT_WIFI_DISCONNECT   = 3,
    ESP_AT_EVENT_MQTT_CONNECTED    = 4,
    ESP_AT_EVENT_MQTT_DISCONNECTED = 5,
    ESP_AT_EVENT_MQTT_MESSAGE      = 6,
    ESP_AT_EVENT_MQTT_PUB_OK       = 7,
    ESP_AT_EVENT_MQTT_PUB_FAIL     = 8,
    ESP_AT_EVENT_HTTP_DONE         = 9,
    ESP_AT_EVENT_ERROR             = 10,
    ESP_AT_EVENT_MAX
} esp_at_event_t;

// 事件 payload
typedef struct {
    esp_at_event_t   type;
    int32_t          link_id;       // MQTT link id；其他事件忽略
    uint32_t         err_code;      // ERR CODE 或 +CWJAP 错误码
    const char      *topic;         // MQTT_MESSAGE：订阅主题
    uint16_t         topic_len;
    const uint8_t   *data;          // MQTT_MESSAGE：负载 / HTTP_DONE：响应 body
    uint16_t         data_len;
    uint16_t         http_status;   // HTTP_DONE：HTTP/1.1 状态码
    void            *user;          // 透传用户上下文
} esp_at_event_payload_t;

// 事件回调签名
typedef void (*esp_at_event_cb_t)(const esp_at_event_payload_t *e, void *user);

// WiFi 状态（cached + AT+CWSTATE? 两路都用）
typedef enum {
    ESP_AT_WIFI_IDLE          = 0,
    ESP_AT_WIFI_CONNECTING    = 1,   // 仅 AT+CWSTATE? 反映（ESP32 正在 join）
    ESP_AT_WIFI_CONNECTED     = 2,   // WIFI CONNECTED URC 已收
    ESP_AT_WIFI_GOT_IP        = 3,   // WIFI GOT IP URC 已收
    ESP_AT_WIFI_LOST          = 4,   // WIFI DISCONNECT URC 已收 / AT+CWSTATE? 4（disconnecting）
} esp_at_wifi_state_t;

#endif /* ESP_AT_TYPES_H */