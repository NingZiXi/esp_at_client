/**
 * @file    esp_at_wifi.c
 * @brief   WiFi 服务封装：CWMODE / CWJAP / CWQAP / CWSTATE? / CIPSTA?
 */

#include "esp_at_wifi.h"
#include "esp_at_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "stm_log.h"

#define ESP_AT_WIFI_TAG "wifi"

// 设置 WiFi 模式。
esp_at_err_t esp_at_wifi_init(uint8_t mode)
{
    char line[24];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line, "AT+CWMODE=%u", mode);
    esp_at_err_t e = esp_at_client_send_sync(line, &r, 3000);
    if (e != ESP_AT_OK) {
        LOGE(ESP_AT_WIFI_TAG, "CWMODE failed: %s", r.text);
    } else {
        LOGI(ESP_AT_WIFI_TAG, "CWMODE=%u ok", mode);
    }
    return e;
}

// 连接 AP。
esp_at_err_t esp_at_wifi_connect(const char *ssid, const char *pwd, uint32_t timeout_ms)
{
    if (!ssid) return ESP_AT_ERR_INVALID_ARG;
    char line[160];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line, "AT+CWJAP=\"%s\",\"%s\"", ssid, pwd ? pwd : "");
    esp_at_err_t e = esp_at_client_send_sync(line, &r, timeout_ms);
    if (e == ESP_AT_OK) {
        LOGI(ESP_AT_WIFI_TAG, "CWJAP ok -> WIFI GOT IP");
    } else {
        LOGW(ESP_AT_WIFI_TAG, "CWJAP failed (status=%d, %u ms): %s",
             r.status, r.elapsed_ms, r.text);
    }
    return e;
}

// 断开 AP。
esp_at_err_t esp_at_wifi_disconnect(void)
{
    at_cmd_response_t r = {0};
    esp_at_err_t e = esp_at_client_send_sync("AT+CWQAP", &r, 5000);
    return e;
}

// 查询 WiFi 状态。
esp_at_wifi_state_t esp_at_wifi_get_state(void)
{
    return esp_at_client_get()->wifi_state;
}

// 主动发 AT+CWSTATE? 查询 ESP32 当前状态（不依赖缓存的 URC）。
esp_at_err_t esp_at_wifi_query_state(esp_at_wifi_query_t *q, uint32_t timeout_ms)
{
    if (!q) return ESP_AT_ERR_INVALID_ARG;
    memset(q, 0, sizeof *q);

    at_cmd_response_t r = {0};
    esp_at_err_t e = esp_at_client_send_sync("AT+CWSTATE?", &r, timeout_ms);
    if (e != ESP_AT_OK) {
        LOGW(ESP_AT_WIFI_TAG, "CWSTATE? failed: %d", (int)e);
        return e;
    }

    // +CWSTATE:<state>,"<ssid>"。
    const char *p = strstr(r.text, "+CWSTATE:");
    if (!p) return ESP_AT_ERR_RESP;
    p += strlen("+CWSTATE:");
    int state = (int)strtol(p, NULL, 10);
    /* ESP-AT CWSTATE 与库内枚举的数值不同，必须显式转换：
       0=未启动，1=已关联但无 IPv4，2=已获取 IPv4，
       3=连接/重连中，4=已断开。 */
    switch (state) {
        case 0: q->state = ESP_AT_WIFI_IDLE;       break;
        case 1: q->state = ESP_AT_WIFI_CONNECTED;  break;
        case 2: q->state = ESP_AT_WIFI_GOT_IP;     break;
        case 3: q->state = ESP_AT_WIFI_CONNECTING; break;
        case 4: q->state = ESP_AT_WIFI_LOST;       break;
        default: return ESP_AT_ERR_RESP;
    }

// SSID 位于逗号后的第一个 "..." 中。
    const char *q1 = strchr(p, ',');
    if (q1) {
        const char *q2 = strchr(q1 + 1, '"');
        const char *q3 = q2 ? strchr(q2 + 1, '"') : NULL;
        if (q2 && q3 && q3 > q2 + 1) {
            size_t len = (size_t)(q3 - q2 - 1);
            if (len >= sizeof q->ssid) len = sizeof q->ssid - 1;
            memcpy(q->ssid, q2 + 1, len);
            q->ssid[len] = '\0';
        }
    }
    return ESP_AT_OK;
}

// 查询 IP / GW / MASK（CIPSTA? 响应包含 ip / gateway / netmask 多行）。
esp_at_err_t esp_at_wifi_get_ip(char ip[16], char gw[16], char mask[16])
{
    at_cmd_response_t r = {0};
    esp_at_err_t e = esp_at_client_send_sync("AT+CIPSTA?", &r, 3000);
    if (e != ESP_AT_OK) {
        LOGE(ESP_AT_WIFI_TAG, "CIPSTA sync failed: %d", (int)e);
        return e;
    }

    if (ip)   ip[0]   = '\0';
    if (gw)   gw[0]   = '\0';
    if (mask) mask[0] = '\0';

    char *save = NULL;
    char *tok  = strtok_r(r.text, "\r\n", &save);
    while (tok) {
        // n 必须严格等于 pattern 长度（含引号），否则会越界比较。
        if      (strncmp(tok, "+CIPSTA:ip:\"",      12) == 0) esp_at_extract_str(tok, "ip",      ip,   ip   ? 16 : 1);
        else if (strncmp(tok, "+CIPSTA:gateway:\"", 17) == 0) esp_at_extract_str(tok, "gateway", gw,   gw   ? 16 : 1);
        else if (strncmp(tok, "+CIPSTA:netmask:\"", 17) == 0) esp_at_extract_str(tok, "netmask", mask, mask ? 16 : 1);
        tok = strtok_r(NULL, "\r\n", &save);
    }
    return ESP_AT_OK;
}
