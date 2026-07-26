/**
 * @file    http_demo.c
 * @brief   ESP-AT 客户端库 HTTP GET / POST 参考示例
 */

#include "esp_at_client.h"
#include "esp_at_wifi.h"
#include "esp_at_http.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "stm_log.h"

#define DEMO_WIFI_SSID        "your-ssid"
#define DEMO_WIFI_PSK         "your-password"
#define DEMO_HTTP_GET_URL     "http://your-server/get"
#define DEMO_HTTP_POST_URL    "http://your-server/post"
#define DEMO_HTTP_TIMEOUT_MS  5000

static const char *TAG = "http_demo";

static void on_any_event(const esp_at_event_payload_t *e, void *user)
{
    (void)user;
    switch (e->type) {
    case ESP_AT_EVENT_WIFI_GOT_IP:
        LOGI(TAG, "WiFi got IP");
        break;
    case ESP_AT_EVENT_WIFI_DISCONNECT:
        LOGW(TAG, "WiFi disconnected (err=%lu)", e->err_code);
        break;
    case ESP_AT_EVENT_HTTP_DONE:
        LOGI(TAG, "HTTP done: body_len=%u", (unsigned)e->data_len);
        break;
    default:
        break;
    }
}

// 调用方负责 free resp.body
static void run_http_get(void)
{
    esp_at_http_resp_t resp = {0};
    esp_at_err_t e = esp_at_http_get(DEMO_HTTP_GET_URL, &resp, DEMO_HTTP_TIMEOUT_MS);
    if (e != ESP_AT_OK) {
        LOGE(TAG, "GET failed: rc=%d", (int)e);
        return;
    }
    LOGI(TAG, "GET %s body_len=%u elapsed=%u ms",
         DEMO_HTTP_GET_URL, resp.body_len, (unsigned)resp.elapsed_ms);
    if (resp.body) {
        LOGI(TAG, "body: %.*s", resp.body_len, (char *)resp.body);
        vPortFree(resp.body);
    }
}

// POST body 里如果有双引号，esp_at_http_request 内部会自动转义为 \"
static void run_http_post(void)
{
    const char *body = "{\"device\":\"stm32\",\"value\":42}";
    esp_at_http_resp_t resp = {0};
    esp_at_err_t e = esp_at_http_post(DEMO_HTTP_POST_URL, "application/json",
                                      (const uint8_t *)body, strlen(body),
                                      &resp, DEMO_HTTP_TIMEOUT_MS);
    if (e != ESP_AT_OK) {
        LOGE(TAG, "POST failed: rc=%d", (int)e);
        return;
    }
    LOGI(TAG, "POST %s body_len=%u elapsed=%u ms",
         DEMO_HTTP_POST_URL, resp.body_len, (unsigned)resp.elapsed_ms);
    if (resp.body) {
        LOGI(TAG, "body: %.*s", resp.body_len, (char *)resp.body);
        vPortFree(resp.body);
    }
}

/**
 * @brief HTTP demo 入口
 */
void app_esp_at_http_demo_run(const esp_at_port_config_t *port_cfg)
{
    esp_at_register_event_cb(ESP_AT_EVENT_ANY, on_any_event, NULL);
    if (esp_at_init(port_cfg) != ESP_AT_OK) {
        LOGE(TAG, "esp_at_init failed");
        return;
    }
    if (esp_at_wifi_init(1) != ESP_AT_OK) {
        LOGE(TAG, "wifi_init failed");
        return;
    }
    if (esp_at_wifi_connect(DEMO_WIFI_SSID, DEMO_WIFI_PSK, 15000) != ESP_AT_OK) {
        LOGE(TAG, "wifi_connect failed");
        return;
    }

    run_http_get();
    run_http_post();

    LOGI(TAG, "HTTP demo done");
}