/**
 * @file    esp_at_client.h
 * @brief   ESP-AT 客户端对外总入口
 */

#ifndef ESP_AT_CLIENT_H
#define ESP_AT_CLIENT_H

#include "esp_at_types.h"
#include "esp_at_port_stm32.h"

#if ESP_AT_ENABLE

/**
 * @brief 初始化 ESP-AT 客户端（建 link / client / 启动 UART DMA / 软件复位 ESP32）
 *
 * @param port_cfg  端口配置（huart / hdma_rx / hdma_tx / en / rst / baud）
 * @return ESP_AT_OK 表示成功
 */
esp_at_err_t       esp_at_init  (const esp_at_port_config_t *port_cfg);

/**
 * @brief 反初始化 ESP-AT 客户端
 *
 * @return ESP_AT_OK 表示成功
 */
esp_at_err_t       esp_at_deinit(void);

/**
 * @brief 同步等待 ESP-AT ready URC
 *
 * @param timeout_ms  等待超时（ms）
 * @return 实际等待时间（ms）
 */
uint32_t           esp_at_wait_ready(uint32_t timeout_ms);

/**
 * @brief 注册事件回调
 *
 * @param evt   事件类型（支持 ESP_AT_EVENT_ANY 通配）
 * @param cb    回调函数
 * @param user  透传给回调的用户上下文
 * @return ESP_AT_OK 表示成功
 */
esp_at_err_t       esp_at_register_event_cb  (esp_at_event_t evt, esp_at_event_cb_t cb, void *user);

/**
 * @brief 反注册事件回调
 *
 * @param evt  事件类型
 * @param cb   要注销的回调（必须与注册时同一指针）
 * @return ESP_AT_OK 表示成功
 */
esp_at_err_t       esp_at_unregister_event_cb(esp_at_event_t evt, esp_at_event_cb_t cb);

/**
 * @brief 同步发送 AT 命令并等待响应
 *
 * @param cmd_line    AT 命令（不含 CRLF）
 * @param resp        响应填充结构
 * @param timeout_ms  超时（ms，0 = ESP_AT_CMD_TIMEOUT_DEFAULT_MS）
 * @return ESP_AT_OK / ERR_TIMEOUT / ERR_BUSY 等
 */
esp_at_err_t       esp_at_cmd_send_sync (const char *cmd_line,
                                        at_cmd_response_t *resp,
                                        uint32_t timeout_ms);

/**
 * @brief 仅发送 AT 命令不等响应（探测用）
 *
 * @param cmd_line    AT 命令
 * @param timeout_ms  写超时（ms）
 * @return ESP_AT_OK / ERR_FAIL
 */
esp_at_err_t       esp_at_cmd_send_only (const char *cmd_line, uint32_t timeout_ms);

#else  /* ESP_AT_ENABLE */

static inline esp_at_err_t esp_at_init(const esp_at_port_config_t *cfg) { (void)cfg; return ESP_AT_OK; }
static inline esp_at_err_t esp_at_deinit(void) { return ESP_AT_OK; }

#endif /* ESP_AT_ENABLE */

#endif /* ESP_AT_CLIENT_H */