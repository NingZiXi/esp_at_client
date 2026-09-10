/**
 * @file    esp_at_wifi.h
 * @brief   ESP-AT WiFi 服务封装：CWMODE / CWJAP / CWQAP / CWSTATE? / CIPSTA?
 */

#ifndef ESP_AT_WIFI_H
#define ESP_AT_WIFI_H

#include "esp_at_types.h"

#if ESP_AT_ENABLE

// AT+CWSTATE? 查询结果
typedef struct {
    esp_at_wifi_state_t state;     // ESP32 实际状态
    char               ssid[33];   // 仅 state>=1 有意义；\0 结尾
} esp_at_wifi_query_t;

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
 * @brief 查询当前 WiFi 状态（cached，URC 驱动更新）
 *
 * 警告：STM32 boot 之前 ESP32 自动重连的 URC 可能丢失，
 * boot 后立即调用返回 ESP_AT_WIFI_IDLE，但 ESP32 实际已 GOT_IP。
 * 需要权威值请用 esp_at_wifi_query_state()。
 *
 * @return esp_at_wifi_state_t
 */
esp_at_wifi_state_t  esp_at_wifi_get_state(void);

/**
 * @brief 主动发 AT+CWSTATE? 查 ESP32 当前状态（不依赖 cached URC）
 *
 * ESP-AT 返回状态码 2（已获取 IPv4）时，库会映射为
 * ESP_AT_WIFI_GOT_IP；状态码 4 表示已断开。
 *
 * 用于 boot 后立即判断 ESP32 是否已自动重连上 AP，
 * 避免对已连的 ESP32 主动 CWJAP 打断内部状态机。
 *
 * @param q          状态填充
 * @param timeout_ms 超时
 * @return ESP_AT_OK / ERR_TIMEOUT / ERR_RESP
 */
esp_at_err_t         esp_at_wifi_query_state(esp_at_wifi_query_t *q, uint32_t timeout_ms);

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
