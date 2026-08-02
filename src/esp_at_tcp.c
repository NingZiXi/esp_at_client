/**
 * @file    esp_at_tcp.c
 * @brief   raw TCP + 自拼 HTTP/1.1 客户端（绕开 ESP-AT HTTPCLIENT state machine bug）
 */

#include "esp_at_tcp.h"
#include "esp_at_client.h"
#include "esp_at_internal.h"
#include "esp_at_link.h"
#include "ringbuffer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "stm32f4xx_hal.h"             // HAL_GetTick
#include "cmsis_os2.h"                 // osDelay
#include "stm_log.h"

#define ESP_AT_TCP_TAG       "tcp"
#define ESP_AT_TCP_LINK_ID   1           // MQTT 占用 link_id=0，新 TCP 用 1，多连接模式

// 查找 HTTP 响应头结束位置
static int http_find_header_end(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i + 3 < len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' &&
            data[i + 2] == '\r' && data[i + 3] == '\n') {
            return (int)(i + 4);
        }
    }
    return -1;
}

// 解析 HTTP Content-Length
static int http_parse_content_length(const uint8_t *data, uint16_t header_len)
{
    static const char key[] = "Content-Length:";
    for (uint16_t i = 0; i + sizeof key - 1 < header_len; i++) {
        if (memcmp(data + i, key, sizeof key - 1) == 0) {
            const char *p = (const char *)data + i + sizeof key - 1;
            return atoi(p);
        }
    }
    return -1;
}

// 等 rx_rb 出现 '>' 单字符
static bool rx_wait_gt_prompt(uint32_t timeout_ms)
{
    ringbuffer_t *rb = &esp_at_client_get()->rx_rb;
    uint32_t deadline = HAL_GetTick() + timeout_ms;
    while (HAL_GetTick() < deadline) {
        if (ringbuffer_find_char(rb, '>') >= 0) {
            uint8_t b;
            ringbuffer_read(rb, &b, 1);
            if (b == '>') return true;
        }
        osDelay(1);
    }
    return false;
}

esp_at_err_t esp_at_tcp_connect(esp_at_tcp_t *tcp,
                                 const char *host, uint16_t port,
                                 uint32_t timeout_ms)
{
    if (!tcp || !host) return ESP_AT_ERR_INVALID_ARG;
    memset(tcp, 0, sizeof *tcp);
    strncpy(tcp->host, host, sizeof tcp->host - 1);
    tcp->port = port;
    tcp->link_id = ESP_AT_TCP_LINK_ID;
    tcp->connected = false;

    // 启多连接模式（一次性，static 记下），MQTT 占用 link_id=0
    static bool s_mux_enabled = false;
    if (!s_mux_enabled) {
        at_cmd_response_t r = {0};
        if (esp_at_cmd_send_sync("AT+CIPMUX=1", &r, 1000) == ESP_AT_OK) {
            s_mux_enabled = true;
            LOGI(ESP_AT_TCP_TAG, "CIPMUX=1 enabled (multi-connection)");
        }
    }

    // 关 link_id=N 上的旧连接（如果残留）
    {
        char close_cmd[24];
        snprintf(close_cmd, sizeof close_cmd, "AT+CIPCLOSE=%u", (unsigned)tcp->link_id);
        at_cmd_response_t r = {0};
        (void)esp_at_cmd_send_sync(close_cmd, &r, 1000);    // 忽略失败
    }

    char line[128];
    snprintf(line, sizeof line, "AT+CIPSTART=%u,\"TCP\",\"%s\",%u",
             (unsigned)tcp->link_id, host, (unsigned)port);
    LOGI(ESP_AT_TCP_TAG, ">> %s", line);

    at_cmd_response_t r = {0};
    esp_at_err_t e = esp_at_cmd_send_sync(line, &r, timeout_ms);
    if (e != ESP_AT_OK) {
        LOGW(ESP_AT_TCP_TAG, "CIPSTART failed: %s", r.text);
        return e;
    }

    char expect[16];
    snprintf(expect, sizeof expect, "%u,CONNECT", (unsigned)tcp->link_id);
    if (!strstr(r.text, expect)) {
        LOGW(ESP_AT_TCP_TAG, "CIPSTART no CONNECT in resp: %s", r.text);
        return ESP_AT_ERR_FAIL;
    }

    tcp->connected = true;
    LOGI(ESP_AT_TCP_TAG, "connected to %s:%u", host, (unsigned)port);
    return ESP_AT_OK;
}

esp_at_err_t esp_at_tcp_close(esp_at_tcp_t *tcp)
{
    if (!tcp) return ESP_AT_ERR_INVALID_ARG;
    if (!tcp->connected) return ESP_AT_OK;

    char line[32];
    snprintf(line, sizeof line, "AT+CIPCLOSE=%u", (unsigned)tcp->link_id);

    at_cmd_response_t r = {0};
    esp_at_err_t e = esp_at_cmd_send_sync(line, &r, 3000);
    tcp->connected = false;
    LOGI(ESP_AT_TCP_TAG, "close rc=%d text=%s", (int)e, r.text);
    return e;
}

esp_at_err_t esp_at_tcp_http_get_range(esp_at_tcp_t *tcp,
                                       const char *host, const char *path,
                                       uint32_t offset, uint32_t len,
                                       uint32_t timeout_ms)
{
    if (!tcp || !tcp->connected || !host || !path) return ESP_AT_ERR_INVALID_ARG;

    // 拼 HTTP/1.1 GET 请求
    char req[256];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Range: bytes=%lu-%lu\r\n"
                     "Connection: keep-alive\r\n"
                     "\r\n",
                     path, host,
                     (unsigned long)offset, (unsigned long)(offset + len - 1));
    if (n <= 0 || n >= (int)sizeof req) return ESP_AT_ERR_INVALID_ARG;

    LOGI(ESP_AT_TCP_TAG, "HTTP GET %s (range=%lu-%lu, %d bytes)",
         path, (unsigned long)offset, (unsigned long)(offset + len - 1), n);

    esp_at_client_t *c = esp_at_client_get();
    c->ipd_len = 0;
    c->ipd_consumed = 0;

    // send_only 发 AT+CIPSEND=<link_id>,<length>
    char cmd[32];
    snprintf(cmd, sizeof cmd, "AT+CIPSEND=%u,%u", (unsigned)tcp->link_id, (unsigned)n);
    LOGI(ESP_AT_TCP_TAG, ">> %s", cmd);
    esp_at_err_t e = esp_at_cmd_send_only(cmd, 1000);
    if (e != ESP_AT_OK) {
        LOGW(ESP_AT_TCP_TAG, "CIPSEND send_only failed");
        return e;
    }

    // 等 '>' prompt（直接在 rx_rb 找）
    if (!rx_wait_gt_prompt(3000)) {
        LOGW(ESP_AT_TCP_TAG, "no '>' prompt in rx_rb");
        return ESP_AT_ERR_TIMEOUT;
    }

    // '>' 收到，link.write HTTP 请求
    esp_at_link_t *link = esp_at_client_get()->link;
    int rc = ESP_AT_LINK_WRITE(link, (const uint8_t *)req, (uint16_t)n, 5000);
    if (rc < 0) {
        LOGW(ESP_AT_TCP_TAG, "link.write failed rc=%d", rc);
        return ESP_AT_ERR_FAIL;
    }

    // 给 ESP-AT 20ms 发送 SEND OK，避免 rx_task 抢先消费
    osDelay(20);
    // 不等 SEND OK：server 已收到 GET = HTTP 请求已经成功发出，直接进入 recv_body
    return ESP_AT_OK;
}

esp_at_err_t esp_at_tcp_recv_body(esp_at_tcp_t *tcp,
                                   uint8_t *out, uint32_t want,
                                   uint32_t *got, uint32_t timeout_ms)
{
    if (!tcp || !out || !got) return ESP_AT_ERR_INVALID_ARG;
    *got = 0;

    esp_at_client_t *c = esp_at_client_get();
    uint32_t deadline = HAL_GetTick() + timeout_ms;

    // 等 HTTP 头完整，再按 Content-Length 等完整响应
    int header_end = -1;
    int content_len = -1;
    uint32_t expected = 0;
    while (HAL_GetTick() < deadline) {
        uint16_t buffered = c->ipd_len;
        if (header_end < 0) {
            header_end = http_find_header_end(c->ipd_buf, buffered);
            if (header_end >= 0) {
                content_len = http_parse_content_length(c->ipd_buf, (uint16_t)header_end);
                if (content_len < 0) {
                    LOGW(ESP_AT_TCP_TAG, "HTTP response has no Content-Length");
                    return ESP_AT_ERR_RESP;
                }
                expected = (uint32_t)header_end + (uint32_t)content_len;
                if (expected > sizeof c->ipd_buf || expected > want) {
                    LOGW(ESP_AT_TCP_TAG, "HTTP response too large: %lu", (unsigned long)expected);
                    return ESP_AT_ERR_NO_MEM;
                }
            }
        }
        if (header_end >= 0 && buffered >= expected) break;
        osDelay(1);
    }

    uint32_t avail = c->ipd_len;
    if (header_end < 0 || avail < expected) {
        LOGW(ESP_AT_TCP_TAG, "incomplete HTTP response: got=%lu expected=%lu",
             (unsigned long)avail, (unsigned long)expected);
        return ESP_AT_ERR_TIMEOUT;
    }

    memcpy(out, c->ipd_buf, expected);
    c->ipd_len = 0;
    c->ipd_consumed = 0;
    *got = expected;
    return ESP_AT_OK;
}