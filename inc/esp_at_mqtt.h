/**
 * @file    esp_at_mqtt.h
 * @brief   ESP-AT MQTT 服务封装（LinkID 固定 0）：
 *         MQTTUSERCFG / MQTTCONNCFG / MQTTCONN / MQTTSUB / MQTTUNSUB /
 *         MQTTPUB / MQTTPUBRAW / MQTTCLEAN
 */

#ifndef ESP_AT_MQTT_H
#define ESP_AT_MQTT_H

#include "esp_at_types.h"

#if ESP_AT_ENABLE

typedef struct {
    uint8_t      link_id;            // 固定 0
    uint32_t     scheme;             // 1=TCP 2=SSL 3=WS 6=WS over TCP
    const char  *client_id;
    const char  *username;           // 可 NULL
    const char  *password;           // 可 NULL
    uint8_t      disable_clean_session;
    const char  *lwt_topic;          // 可 NULL
    const char  *lwt_msg;            // 可 NULL
    uint8_t      lwt_qos;
    uint8_t      lwt_retain;
    uint16_t     keepalive_s;
} esp_at_mqtt_user_cfg_t;

typedef struct {
    uint8_t   link_id;
    uint32_t  timeout_s;
    uint8_t   ssl;                   // 0/1
    const char *path;                // WS path，NULL → ""
} esp_at_mqtt_conn_cfg_t;

typedef void (*esp_at_mqtt_data_cb_t)(uint8_t link_id,
                                      const char *topic, uint16_t topic_len,
                                      const uint8_t *data, uint16_t data_len,
                                      void *user);

/**
 * @brief MQTT 初始化（占位）
 *
 * @return ESP_AT_OK
 */
esp_at_err_t esp_at_mqtt_init      (void);

/**
 * @brief MQTT 连接（USERCFG + CONNCFG + CONN，内部已等 2s 让 broker 注册订阅）
 *
 * @param uc         USERCFG
 * @param cc         CONNCFG
 * @param host       broker host
 * @param port       broker port
 * @param timeout_ms 超时
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_mqtt_connect   (const esp_at_mqtt_user_cfg_t *uc,
                                    const esp_at_mqtt_conn_cfg_t *cc,
                                    const char *host, uint16_t port,
                                    uint32_t timeout_ms);

/**
 * @brief MQTT 发布（小 payload 走 PUB 通道，>200B 切到 RAW）
 *
 * @param link_id    固定 0
 * @param topic      主题
 * @param data       数据
 * @param len        长度
 * @param qos        QoS
 * @param retain     retain
 * @param timeout_ms 超时
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_mqtt_publish   (uint8_t link_id, const char *topic,
                                    const uint8_t *data, uint16_t len,
                                    uint8_t qos, uint8_t retain,
                                    uint32_t timeout_ms);

/**
 * @brief MQTT 长 payload 发布（MQTTPUBRAW）
 *
 * @param link_id    固定 0
 * @param topic      主题
 * @param data       数据
 * @param len        长度
 * @param qos        QoS
 * @param retain     retain
 * @param timeout_ms 超时
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_mqtt_publish_raw(uint8_t link_id, const char *topic,
                                     const uint8_t *data, uint16_t len,
                                     uint8_t qos, uint8_t retain,
                                     uint32_t timeout_ms);

/**
 * @brief MQTT 订阅（下行通过 ESP_AT_EVENT_MQTT_MESSAGE 回调投递）
 *
 * @param link_id    固定 0
 * @param topic      主题
 * @param qos        QoS
 * @param cb         回调（未使用，保留兼容）
 * @param user       用户上下文（未使用）
 * @param timeout_ms 超时
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_mqtt_subscribe (uint8_t link_id, const char *topic, uint8_t qos,
                                    esp_at_mqtt_data_cb_t cb, void *user,
                                    uint32_t timeout_ms);

/**
 * @brief MQTT 取消订阅
 *
 * @param link_id    固定 0
 * @param topic      主题
 * @param timeout_ms 超时
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_mqtt_unsubscribe(uint8_t link_id, const char *topic, uint32_t timeout_ms);

/**
 * @brief MQTT 断开（MQTTCLEAN）
 *
 * @param link_id  固定 0
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_mqtt_disconnect(uint8_t link_id);

/**
 * @brief 查询 MQTT 连接状态
 *
 * @return true 已连接
 */
bool         esp_at_mqtt_is_connected(void);

#else
static inline esp_at_err_t esp_at_mqtt_init(void) { return ESP_AT_OK; }
#endif

#endif /* ESP_AT_MQTT_H */