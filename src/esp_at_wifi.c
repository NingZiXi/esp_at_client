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

// 设置 WiFi 模式
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

// 连接 AP
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

// 断开 AP
esp_at_err_t esp_at_wifi_disconnect(void)
{
    at_cmd_response_t r = {0};
    esp_at_err_t e = esp_at_client_send_sync("AT+CWQAP", &r, 5000);
    return e;
}

// 查询 WiFi 状态
esp_at_wifi_state_t esp_at_wifi_get_state(void)
{
    return esp_at_client_get()->wifi_state;
}

// 查询 IP / GW / MASK（CIPSTA? 响应多行：ip / gateway / netmask）
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
        // n 必须严格等于 pattern 长度（含"），否则越界比较
        if      (strncmp(tok, "+CIPSTA:ip:\"",      12) == 0) esp_at_extract_str(tok, "ip",      ip,   ip   ? 16 : 1);
        else if (strncmp(tok, "+CIPSTA:gateway:\"", 17) == 0) esp_at_extract_str(tok, "gateway", gw,   gw   ? 16 : 1);
        else if (strncmp(tok, "+CIPSTA:netmask:\"", 17) == 0) esp_at_extract_str(tok, "netmask", mask, mask ? 16 : 1);
        tok = strtok_r(NULL, "\r\n", &save);
    }
    return ESP_AT_OK;
}