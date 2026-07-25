/**
 * @file    esp_at_mqtt.c
 * @brief   MQTT 服务封装（LinkID 固定 0）：USERCFG / CONNCFG / CONN / SUB / UNSUB / PUB / PUBRAW / CLEAN
 */

#include "esp_at_mqtt.h"
#include "esp_at_internal.h"
#include "esp_at_client.h"

#include <stdio.h>
#include <string.h>

#include "stm_log.h"

#define ESP_AT_MQTT_TAG "mqtt"

// MQTT 初始化（占位）
esp_at_err_t esp_at_mqtt_init(void)
{
    return ESP_AT_OK;
}

// MQTT 连接：残留清理 → USERCFG → CONNCFG → CONN + 等 2s 让 broker 注册订阅
esp_at_err_t esp_at_mqtt_connect(const esp_at_mqtt_user_cfg_t *uc,
                                 const esp_at_mqtt_conn_cfg_t *cc,
                                 const char *host, uint16_t port,
                                 uint32_t timeout_ms)
{
    if (!uc || !host) return ESP_AT_ERR_INVALID_ARG;

    char line[256];
    at_cmd_response_t r = {0};

    {
        esp_at_err_t ce = esp_at_cmd_send_sync("AT+MQTTCLEAN=0", &r, 5000);  // 残留清理：上次没正常 CLEAN 时此步必要
        LOGI(ESP_AT_MQTT_TAG, "MQTTCLEAN pre-clean: rc=%d", (int)ce);
    }

    snprintf(line, sizeof line,                                              // USERCFG 共 8 段（4.1.x），LWT 不在此
             "AT+MQTTUSERCFG=0,%lu,\"%s\",\"%s\",\"%s\",0,0,\"\"",
             (unsigned long)uc->scheme,
             uc->client_id ? uc->client_id : "",
             uc->username ? uc->username : "",
             uc->password ? uc->password : "");
    esp_at_err_t e = esp_at_cmd_send_sync(line, &r, 3000);
    if (e != ESP_AT_OK) {
        LOGE(ESP_AT_MQTT_TAG, "MQTTUSERCFG failed: rc=%d status=%d text=[%.*s]",
             (int)e, (int)r.status, (int)r.text_len, r.text);
        return e;
    }
    LOGI(ESP_AT_MQTT_TAG, "USERCFG ok");

    HAL_Delay(200);                                                            // 模块状态机切换余量

    uint16_t keepalive = cc ? cc->timeout_s : uc->keepalive_s;
    if (keepalive == 0) keepalive = uc->keepalive_s ? uc->keepalive_s : 120;   // ESP 强制默认 120s
    snprintf(line, sizeof line,
             "AT+MQTTCONNCFG=0,%u,%u,\"%s\",\"%s\",%u,%u",
             keepalive, uc->disable_clean_session,
             uc->lwt_topic ? uc->lwt_topic : "",
             uc->lwt_msg ? uc->lwt_msg : "",
             uc->lwt_qos, uc->lwt_retain);
    e = esp_at_client_send_sync(line, &r, 5000);
    if (e != ESP_AT_OK) {
        LOGE(ESP_AT_MQTT_TAG, "MQTTCONNCFG failed: rc=%d status=%d text=[%.*s]",
             (int)e, (int)r.status, (int)r.text_len, r.text);
        return e;
    }
    LOGI(ESP_AT_MQTT_TAG, "CONNCFG ok");

    snprintf(line, sizeof line,
             "AT+MQTTCONN=0,\"%s\",%u,1", host, (unsigned)port);
    e = esp_at_client_send_sync(line, &r, timeout_ms);
    if (e == ESP_AT_OK) {
        LOGI(ESP_AT_MQTT_TAG, "MQTTCONN to %s:%u ok", host, (unsigned)port);
        /* MQTTCONN OK ≠ broker 真订阅能力 ready；不等会丢 broker 注册 → 下行永不达 */
        LOGI(ESP_AT_MQTT_TAG, "waiting 2s for ESP-AT MQTT state to settle...");
        HAL_Delay(2000);
    } else {
        LOGW(ESP_AT_MQTT_TAG, "MQTTCONN failed: %s", r.text);
    }
    return e;
}

// MQTT 发布（小 payload 走 PUB，>200B 切到 RAW）
esp_at_err_t esp_at_mqtt_publish(uint8_t link_id, const char *topic,
                                 const uint8_t *data, uint16_t len,
                                 uint8_t qos, uint8_t retain,
                                 uint32_t timeout_ms)
{
    if (!topic || (!data && len)) return ESP_AT_ERR_INVALID_ARG;
    if (len > 200) {
        return esp_at_mqtt_publish_raw(link_id, topic, data, len, qos, retain, timeout_ms);
    }

    char line[320];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line,                                              // 二进制 payload 走 publish_raw
             "AT+MQTTPUB=%u,\"%s\",\"%.*s\",%u,%u",
             link_id, topic, (int)len, (const char *)data, qos, retain);
    return esp_at_client_send_sync(line, &r, timeout_ms);
}

// MQTT 长 payload 发布（MQTTPUBRAW）
esp_at_err_t esp_at_mqtt_publish_raw(uint8_t link_id, const char *topic,
                                     const uint8_t *data, uint16_t len,
                                     uint8_t qos, uint8_t retain,
                                     uint32_t timeout_ms)
{
    if (!topic || !data || len == 0) return ESP_AT_ERR_INVALID_ARG;
    char line[128];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line,
             "AT+MQTTPUBRAW=%u,\"%s\",%u,%u,%u",
             link_id, topic, (unsigned)len, qos, retain);

    // TODO: DATA_PROMPT ('>') 处理：先 link.write 再等 +MQTTPUB:OK/FAIL
    esp_at_err_t e = esp_at_client_send_sync(line, &r, timeout_ms);
    if (e == ESP_AT_OK) {
        extern esp_at_err_t esp_at_port_uart_transmit(const uint8_t *data, uint16_t size, uint32_t timeout_ms);
        esp_at_port_uart_transmit(data, len, timeout_ms);
    }
    return e;
}

// MQTT 订阅（下行通过 ESP_AT_EVENT_MQTT_MESSAGE 回调）
esp_at_err_t esp_at_mqtt_subscribe(uint8_t link_id, const char *topic, uint8_t qos,
                                   esp_at_mqtt_data_cb_t cb, void *user,
                                   uint32_t timeout_ms)
{
    (void)cb; (void)user;
    char line[160];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line, "AT+MQTTSUB=%u,\"%s\",%u", link_id, topic, qos);
    return esp_at_client_send_sync(line, &r, timeout_ms);
}

// MQTT 取消订阅
esp_at_err_t esp_at_mqtt_unsubscribe(uint8_t link_id, const char *topic, uint32_t timeout_ms)
{
    char line[160];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line, "AT+MQTTUNSUB=%u,\"%s\"", link_id, topic);
    return esp_at_client_send_sync(line, &r, timeout_ms);
}

// MQTT 断开（MQTTCLEAN）
esp_at_err_t esp_at_mqtt_disconnect(uint8_t link_id)
{
    char line[24];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line, "AT+MQTTCLEAN=%u", link_id);
    return esp_at_client_send_sync(line, &r, 5000);
}

// 查询 MQTT 连接状态
bool esp_at_mqtt_is_connected(void)
{
    return esp_at_client_get()->mqtt_connected;
}