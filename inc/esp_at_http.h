/**
 * @file    esp_at_http.h
 * @brief   ESP-AT HTTP 服务封装：HTTPCLIENT (GET/POST/PUT/DELETE)、HTTPURLCFG（>200B URL）
 */

#ifndef ESP_AT_HTTP_H
#define ESP_AT_HTTP_H

#include "esp_at_types.h"

#if ESP_AT_ENABLE

typedef enum {
    ESP_AT_HTTP_HEAD   = 1,
    ESP_AT_HTTP_GET    = 2,
    ESP_AT_HTTP_POST   = 3,
    ESP_AT_HTTP_PUT    = 4,
    ESP_AT_HTTP_DELETE = 5,
} esp_at_http_method_t;

typedef struct {
    uint16_t   status;        // 解析自响应头 "HTTP/1.1 200 ..."
    uint8_t   *body;          // malloc'd，调用者 free
    uint16_t   body_len;
    uint32_t   elapsed_ms;
} esp_at_http_resp_t;

/**
 * @brief HTTP 通用请求
 *
 * @param method        HTTP 方法
 * @param url           URL
 * @param content_type  MIME（NULL = application/x-www-form-urlencoded）
 * @param body          body 数据
 * @param body_len      body 长度
 * @param resp          响应填充
 * @param timeout_ms    超时
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_http_request (esp_at_http_method_t method,
                                  const char *url,
                                  const char *content_type,
                                  const uint8_t *body, uint16_t body_len,
                                  esp_at_http_resp_t *resp,
                                  uint32_t timeout_ms);

/**
 * @brief HTTP GET 便捷封装
 *
 * @param url         URL
 * @param resp        响应填充
 * @param timeout_ms  超时
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_http_get (const char *url, esp_at_http_resp_t *resp, uint32_t timeout_ms);

/**
 * @brief HTTP POST 便捷封装
 *
 * @param url          URL
 * @param content_type MIME（可 NULL）
 * @param body         body 数据
 * @param body_len     body 长度
 * @param resp         响应填充
 * @param timeout_ms   超时
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_http_post(const char *url, const char *content_type,
                              const uint8_t *body, uint16_t body_len,
                              esp_at_http_resp_t *resp, uint32_t timeout_ms);

#else
static inline esp_at_err_t esp_at_http_get(const char *u, esp_at_http_resp_t *r, uint32_t t) {
    (void)u; (void)r; (void)t; return ESP_AT_OK;
}
#endif

#endif /* ESP_AT_HTTP_H */