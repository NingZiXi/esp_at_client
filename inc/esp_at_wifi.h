/**
 * @file    esp_at_wifi.h
 * @brief   ESP-AT WiFi 服务封装：CWMODE / CWJAP / CWQAP / CWSTATE? / CIPSTA?
 */

#ifndef ESP_AT_WIFI_H
#define ESP_AT_WIFI_H

#include "esp_at_types.h"

#if ESP_AT_ENABLE

/**
 * @brief 设置 WiFi 模式
 *
 * @param mode  1=STA 2=AP 3=STA+AP
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t         esp_at_wifi_init(uint8_t mode);

/**
 * @brief 连接 AP（CWJAP）
 *
 * @param ssid        SSID
 * @param pwd         密码（可 NULL）
 * @param timeout_ms  超时
 * @return ESP_AT_OK / ERR_TIMEOUT
 */
esp_at_err_t         esp_at_wifi_connect(const char *ssid, const char *pwd, uint32_t timeout_ms);

/**
 * @brief 断开 AP（CWQAP）
 *
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t         esp_at_wifi_disconnect(void);

/**
 * @brief 查询当前 WiFi 状态
 *
 * @return esp_at_wifi_state_t
 */
esp_at_wifi_state_t  esp_at_wifi_get_state(void);

/**
 * @brief 查询 IP / 网关 / 子网掩码（CIPSTA?）
 *
 * @param ip[16]   IP 输出
 * @param gw[16]   GW 输出
 * @param mask[16] MASK 输出
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t         esp_at_wifi_get_ip(char ip[16], char gw[16], char mask[16]);

#else
static inline esp_at_err_t esp_at_wifi_init(uint8_t m) { (void)m; return ESP_AT_OK; }
#endif

#endif /* ESP_AT_WIFI_H */