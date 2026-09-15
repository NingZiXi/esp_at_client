/**
 * @file    esp_at_http.c
 * @brief   HTTP 服务封装：HTTPCLIENT (HEAD/GET/POST/PUT/DELETE)、HTTPURLCFG（>200B URL）
 */

#include "esp_at_http.h"
#include "esp_at_client.h"
#include "esp_at_internal.h"
#include "esp_at_link.h"
#include "ringbuffer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "stm_log.h"

#define ESP_AT_HTTP_TAG "http"
#define HTTP_URL_PRESET_THRESHOLD  200

static bool http_wait_data_prompt(uint32_t timeout_ms)
{
    ringbuffer_t *rb = &esp_at_client_get()->rx_rb;
    const uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms) {
        if (esp_at_client_get()->data_prompt_seen) return true;
        const int prompt_pos = ringbuffer_find_char(rb, '>');
        if (prompt_pos >= 0) {
            ringbuffer_discard(rb, (uint16_t)(prompt_pos + 1));
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    return false;
}

static esp_at_err_t http_configure_long_url(const char *url, uint32_t timeout_ms)
{
    char command[48];
    const size_t url_len = strlen(url);
    if (url_len > UINT16_MAX) return ESP_AT_ERR_INVALID_ARG;
    const int written = snprintf(command, sizeof command,
                                 "AT+HTTPURLCFG=%u", (unsigned)url_len);
    if (written <= 0 || written >= (int)sizeof command) {
        return ESP_AT_ERR_INVALID_ARG;
    }

    esp_at_client_t *client = esp_at_client_get();
    client->data_prompt_seen = false;
    client->http_url_set_result = 0;
    /* 初始 OK 之后还会出现 '>'；send_only 避免在调用栈上再叠加一份
       512 字节响应对象，由 prompt 等待逻辑统一消费握手内容。 */
    esp_at_err_t result = esp_at_cmd_send_only(command, 1000U);
    if (result != ESP_AT_OK) return result;
    if (!http_wait_data_prompt(timeout_ms)) return ESP_AT_ERR_TIMEOUT;

    extern esp_at_err_t esp_at_port_uart_transmit(const uint8_t *data,
                                                   uint16_t size,
                                                   uint32_t wait_ms);
    result = esp_at_port_uart_transmit((const uint8_t *)url,
                                       (uint16_t)url_len, timeout_ms);
    if (result != ESP_AT_OK) return result;

    const uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms) {
        if (client->http_url_set_result > 0) return ESP_AT_OK;
        if (client->http_url_set_result < 0) return ESP_AT_ERR_RESP;
        vTaskDelay(pdMS_TO_TICKS(1U));
    }
    return ESP_AT_ERR_TIMEOUT;
}

// 解析 http://主机[:端口]/路径。
static esp_at_err_t parse_url(const char *url,
                              char *scheme, size_t scheme_sz,
                              char *host, size_t host_sz,
                              char *path, size_t path_sz)
{
    const char *p = strstr(url, "://");
    if (!p) return ESP_AT_ERR_INVALID_ARG;
    size_t s_len = (size_t)(p - url);
    if (s_len >= scheme_sz) return ESP_AT_ERR_INVALID_ARG;
    memcpy(scheme, url, s_len);
    scheme[s_len] = '\0';
    p += 3;
    const char *slash = strchr(p, '/');
    const char *colon = strchr(p, ':');
    if (colon && (!slash || colon < slash)) {
        size_t h_len = (size_t)(colon - p);
        if (h_len >= host_sz) return ESP_AT_ERR_INVALID_ARG;
        memcpy(host, p, h_len);
        host[h_len] = '\0';
    } else {
        const char *end = slash ? slash : (p + strlen(p));
        size_t h_len = (size_t)(end - p);
        if (h_len >= host_sz) return ESP_AT_ERR_INVALID_ARG;
        memcpy(host, p, h_len);
        host[h_len] = '\0';
    }
    if (slash) {
        strncpy(path, slash, path_sz - 1);
        path[path_sz - 1] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }
    return ESP_AT_OK;
}

// HTTP 通用请求
esp_at_err_t esp_at_http_request(esp_at_http_method_t method,
                                 const char *url,
                                 const char *content_type,
                                 const uint8_t *body, uint16_t body_len,
                                 esp_at_http_resp_t *resp,
                                 uint32_t timeout_ms)
{
    if (!url || !resp) return ESP_AT_ERR_INVALID_ARG;
    memset(resp, 0, sizeof *resp);
    char scheme[8], host[128], path[160];
    if (parse_url(url, scheme, sizeof scheme, host, sizeof host, path, sizeof path)
        != ESP_AT_OK) return ESP_AT_ERR_INVALID_ARG;

    int ct = 0;                                       // 0=表单 1=JSON 2=多部分 3=XML
    if (content_type) {
        if (strcmp(content_type, "application/json") == 0) ct = 1;
        else if (strcmp(content_type, "multipart/form-data") == 0) ct = 2;
        else if (strcmp(content_type, "text/xml") == 0) ct = 3;
    }

    int transport = strcmp(scheme, "https") == 0 ? 2 : 1;
    LOGD(ESP_AT_PROTO_TAG, "send scheme=%s host=%s path=%s ct=%d transport=%d",
         scheme, host, path, ct, transport);

    char line[512];
    at_cmd_response_t r = {0};
    int n;

    const bool use_url_preset = strlen(url) > HTTP_URL_PRESET_THRESHOLD;
    if (use_url_preset) {
        esp_at_err_t preset_result = http_configure_long_url(url, timeout_ms);
        if (preset_result != ESP_AT_OK) {
            LOGW(ESP_AT_HTTP_TAG, "HTTPURLCFG failed: %d", (int)preset_result);
            return preset_result;
        }
    }
    const char *command_url = use_url_preset ? "" : url;

    // ESP-AT 4.1.x HTTPCLIENT：URL 必须使用双引号，否则 AT 解析器会把 : / 当成字段分隔符。
    if (method == ESP_AT_HTTP_GET || method == ESP_AT_HTTP_HEAD) {
        n = snprintf(line, sizeof line,
                     "AT+HTTPCLIENT=%d,%d,\"%s\",,,%d",
                     (int)method, ct, command_url, transport);
    } else if (body && body_len > 0) {
        // data 中的双引号需要转义（\"），否则 AT 解析器会截断字段。
        char escaped[256];
        size_t ei = 0;
        for (size_t bi = 0; bi < body_len && ei + 2 < sizeof escaped; bi++) {
            if (((const char *)body)[bi] == '"') {
                escaped[ei++] = '\\';
            }
            escaped[ei++] = ((const char *)body)[bi];
        }
        escaped[ei] = '\0';
        n = snprintf(line, sizeof line,
                     "AT+HTTPCLIENT=%d,%d,\"%s\",,,%d,\"%s\"",
                     (int)method, ct, command_url, transport, escaped);
    } else {
        n = snprintf(line, sizeof line,
                     "AT+HTTPCLIENT=%d,%d,\"%s\",,,%d",
                     (int)method, ct, command_url, transport);
    }
    (void)n;
    LOGV(ESP_AT_PROTO_TAG, "<< %s", line);

    esp_at_client_t *client = esp_at_client_get();
    if (client->http_rx_buf) {
        vPortFree(client->http_rx_buf);
    }
    client->http_rx_buf = (uint8_t *)pvPortMalloc(ESP_AT_HTTP_BODY_MAX + 1U);
    if (!client->http_rx_buf) return ESP_AT_ERR_NO_MEM;
    client->http_rx_len = 0U;
    client->http_rx_overflow = false;

    esp_at_err_t e = esp_at_client_send_sync(line, &r, timeout_ms);
    if (use_url_preset) {
        /* 清除一次性 URL；返回的 OK 由 rx_task 消费，不需要占用大响应对象。 */
        (void)esp_at_cmd_send_only("AT+HTTPURLCFG=0", 1000U);
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    if (e != ESP_AT_OK) {
        LOGW(ESP_AT_HTTP_TAG, "HTTPCLIENT failed: %s", r.text);
        vPortFree(client->http_rx_buf);
        client->http_rx_buf = NULL;
        client->http_rx_len = 0U;
        return e;
    }

    if (client->http_rx_overflow) {
        vPortFree(client->http_rx_buf);
        client->http_rx_buf = NULL;
        client->http_rx_len = 0U;
        return ESP_AT_ERR_NO_MEM;
    }
    if (client->http_rx_len > 0U) {
        client->http_rx_buf[client->http_rx_len] = '\0';
        resp->body = client->http_rx_buf;
        resp->body_len = client->http_rx_len;
        client->http_rx_buf = NULL;
        client->http_rx_len = 0U;
    } else {
        vPortFree(client->http_rx_buf);
        client->http_rx_buf = NULL;
    }
    resp->elapsed_ms = r.elapsed_ms;
    return ESP_AT_OK;
}

// HTTP GET 便捷封装
esp_at_err_t esp_at_http_get(const char *url, esp_at_http_resp_t *resp, uint32_t timeout_ms)
{
    return esp_at_http_request(ESP_AT_HTTP_GET, url, NULL, NULL, 0, resp, timeout_ms);
}

// HTTP POST 便捷封装
esp_at_err_t esp_at_http_post(const char *url, const char *content_type,
                              const uint8_t *body, uint16_t body_len,
                              esp_at_http_resp_t *resp, uint32_t timeout_ms)
{
    return esp_at_http_request(ESP_AT_HTTP_POST, url, content_type,
                               body, body_len, resp, timeout_ms);
}
