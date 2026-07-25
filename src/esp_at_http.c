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

// 解析 http://host[:port]/path
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

    int ct = 0;                                       // 0=urlencoded 1=json 2=multipart 3=xml
    if (content_type) {
        if (strcmp(content_type, "application/json") == 0) ct = 1;
        else if (strcmp(content_type, "multipart/form-data") == 0) ct = 2;
        else if (strcmp(content_type, "text/xml") == 0) ct = 3;
    }

    int transport = strcmp(scheme, "https") == 0 ? 2 : 1;

    char line[512];
    at_cmd_response_t r = {0};
    int n;

    // >200B URL 改走 AT+HTTPURLCFG 预存；当前简化直接传 url
    (void)HTTP_URL_PRESET_THRESHOLD;

    if (method == ESP_AT_HTTP_GET || method == ESP_AT_HTTP_HEAD) {
        n = snprintf(line, sizeof line,
                     "AT+HTTPCLIENT=%d,%d,%s,%s,%s,%d",
                     (int)method, ct, url, host, path, transport);
    } else if (body && body_len > 0) {
        n = snprintf(line, sizeof line,
                     "AT+HTTPCLIENT=%d,%d,%s,%s,%s,%d,\"%.*s\"",
                     (int)method, ct, url, host, path, transport,
                     (int)body_len, (const char *)body);
    } else {
        n = snprintf(line, sizeof line,
                     "AT+HTTPCLIENT=%d,%d,%s,%s,%s,%d",
                     (int)method, ct, url, host, path, transport);
    }
    (void)n;

    esp_at_err_t e = esp_at_client_send_sync(line, &r, timeout_ms);
    if (e != ESP_AT_OK) {
        LOGW(ESP_AT_HTTP_TAG, "HTTPCLIENT failed: %s", r.text);
        return e;
    }

    // 多帧 +HTTPCLIENT:<size>,<data>，这里只取最后一帧（完整拼接留下一版）
    char *comma = strrchr(r.text, ',');
    if (comma) {
        int sz = atoi(comma + 1);
        if (sz > 0 && sz < 4096) {
            resp->body = (uint8_t *)pvPortMalloc((uint16_t)(sz + 1));
            if (resp->body) {
                memcpy(resp->body, comma + 1, (uint16_t)sz);
                resp->body[sz] = '\0';
                resp->body_len = (uint16_t)sz;
            }
        }
    }
    resp->status = 200;                               // TODO: 解析响应头第一行
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