/**
 * @file    esp_at_http.c
 * @brief   HTTP 服务封装：HTTPCLIENT (HEAD/GET/POST/PUT/DELETE)、HTTPURLCFG（>200B URL）
 */

#include "esp_at_http.h"
#include "esp_at_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "stm_log.h"

#define ESP_AT_HTTP_TAG "http"
#define HTTP_URL_PRESET_THRESHOLD  200

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

// URL 超过 200 字节时可通过 AT+HTTPURLCFG 预存；当前实现直接传入 URL。
    (void)HTTP_URL_PRESET_THRESHOLD;

    // ESP-AT 4.1.x HTTPCLIENT：URL 必须使用双引号，否则 AT 解析器会把 : / 当成字段分隔符。
    if (method == ESP_AT_HTTP_GET || method == ESP_AT_HTTP_HEAD) {
        n = snprintf(line, sizeof line,
                     "AT+HTTPCLIENT=%d,%d,\"%s\",,,%d",
                     (int)method, ct, url, transport);
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
                     (int)method, ct, url, transport, escaped);
    } else {
        n = snprintf(line, sizeof line,
                     "AT+HTTPCLIENT=%d,%d,\"%s\",,,%d",
                     (int)method, ct, url, transport);
    }
    (void)n;
    LOGV(ESP_AT_PROTO_TAG, "<< %s", line);

    esp_at_err_t e = esp_at_client_send_sync(line, &r, timeout_ms);
    if (e != ESP_AT_OK) {
        LOGW(ESP_AT_HTTP_TAG, "HTTPCLIENT failed: %s", r.text);
        return e;
    }

    // +HTTPCLIENT:<size>,<body>：ESP-AT 只透传 body（状态行和头部已由内部解析），resp->status 保持为 0。
    // TODO：支持长 body 的多帧拼接。
    const char *p_hdr = strstr(r.text, "+HTTPCLIENT:");
    if (p_hdr) {
        const char *p_sz = p_hdr + strlen("+HTTPCLIENT:");
        int sz = atoi(p_sz);
        const char *comma = strchr(p_hdr, ',');
        const char *body_start = comma ? comma + 1 : NULL;
        if (sz > 0 && sz < 4096 && body_start) {
            size_t body_available = (body_start < r.text + r.text_len)
                ? (size_t)((r.text + r.text_len) - body_start) : 0U;
            /* 响应文本可能被截断；禁止按声明长度越界读取。 */
            if ((size_t)sz > body_available) {
                LOGW(ESP_AT_HTTP_TAG,
                     "HTTP body truncated: declared=%d available=%u",
                     sz, (unsigned)body_available);
                return ESP_AT_ERR_RESP;
            }
            resp->body = (uint8_t *)pvPortMalloc((uint16_t)(sz + 1));
            if (resp->body) {
                memcpy(resp->body, body_start, (uint16_t)sz);
                resp->body[sz] = '\0';
                resp->body_len = (uint16_t)sz;
            }
        }
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
