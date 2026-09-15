/**
 * @file    esp_at_mqtt.c
 * @brief   MQTT 服务封装（LinkID 固定 0）：USERCFG / CONNCFG / CONN / SUB / UNSUB / PUB / PUBRAW / CLEAN
 */

#include "esp_at_mqtt.h"
#include "esp_at_internal.h"
#include "esp_at_client.h"
#include "esp_at_link.h"
#include "ringbuffer.h"

#include <stdio.h>
#include <string.h>

#include "stm_log.h"

// CMSIS-RTOS 2 API：HAL_Delay 不会让出 CPU，因此使用该接口主动让出。
extern void osDelay(uint32_t ms);

#define ESP_AT_MQTT_TAG "mqtt"

static bool mqtt_escape_text(char *out, size_t out_size,
                             const uint8_t *input, size_t input_len)
{
    size_t write_pos = 0U;
    if (!out || out_size == 0U || (!input && input_len > 0U)) return false;

    for (size_t i = 0U; i < input_len; ++i) {
        const uint8_t ch = input[i];
        /* ESP-AT 字符串参数中的逗号、双引号和反斜杠必须转义。 */
        if (ch == ',' || ch == '"' || ch == '\\') {
            if (write_pos + 2U >= out_size) return false;
            out[write_pos++] = '\\';
        } else if (ch < 0x20U || ch > 0x7EU) {
            return false;
        } else if (write_pos + 1U >= out_size) {
            return false;
        }
        out[write_pos++] = (char)ch;
    }
    out[write_pos] = '\0';
    return true;
}

static bool mqtt_wait_data_prompt(uint32_t timeout_ms)
{
    ringbuffer_t *rb = &esp_at_client_get()->rx_rb;
    const uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms) {
        if (esp_at_client_get()->data_prompt_seen) return true;
        const int prompt_pos = ringbuffer_find_char(rb, '>');
        if (prompt_pos >= 0) {
            ringbuffer_discard(rb, (uint16_t)(prompt_pos + 1));
            return true;
        }
        osDelay(1U);
    }
    return false;
}

// MQTT 初始化（占位）。
esp_at_err_t esp_at_mqtt_init(void)
{
    return ESP_AT_OK;
}

// MQTT 连接：清理残留 → USERCFG → CONNCFG → CONN，并等待 2 秒让 broker 完成订阅注册。
esp_at_err_t esp_at_mqtt_connect(const esp_at_mqtt_user_cfg_t *uc,
                                 const esp_at_mqtt_conn_cfg_t *cc,
                                 const char *host, uint16_t port,
                                 uint32_t timeout_ms)
{
    if (!uc || !host) return ESP_AT_ERR_INVALID_ARG;

    char line[256];
    at_cmd_response_t r = {0};

    {
        esp_at_err_t ce = esp_at_cmd_send_sync("AT+MQTTCLEAN=0", &r, 5000);  // 清理上次未正常关闭的连接。
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

    osDelay(200);                                                              // 模块状态机切换余量

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
        /* MQTTCONN 返回 OK 不代表 broker 已完成订阅注册；不等待会导致下行消息丢失。 */
        LOGI(ESP_AT_MQTT_TAG, "waiting 2s for ESP-AT MQTT state to settle...");
        HAL_Delay(2000);
    } else {
        LOGW(ESP_AT_MQTT_TAG, "MQTTCONN failed: %s", r.text);
    }
    return e;
}

    // MQTT 发布（小负载走 PUB，>200B 切换到 RAW）。
esp_at_err_t esp_at_mqtt_publish(uint8_t link_id, const char *topic,
                                 const uint8_t *data, uint16_t len,
                                 uint8_t qos, uint8_t retain,
                                 uint32_t timeout_ms)
{
    if (!topic || (!data && len)) return ESP_AT_ERR_INVALID_ARG;
    if (len > 200) {
        return esp_at_mqtt_publish_raw(link_id, topic, data, len, qos, retain, timeout_ms);
    }

    char escaped_topic[160];
    char escaped_data[416];
    if (!mqtt_escape_text(escaped_topic, sizeof escaped_topic,
                          (const uint8_t *)topic, strlen(topic))) {
        return ESP_AT_ERR_INVALID_ARG;
    }
    if (!mqtt_escape_text(escaped_data, sizeof escaped_data, data, len)) {
        return esp_at_mqtt_publish_raw(link_id, topic, data, len,
                                       qos, retain, timeout_ms);
    }

    char line[640];
    at_cmd_response_t r = {0};
    const int written = snprintf(line, sizeof line,
             "AT+MQTTPUB=%u,\"%s\",\"%s\",%u,%u",
             link_id, escaped_topic, escaped_data, qos, retain);
    if (written <= 0 || written >= (int)sizeof line
        || (size_t)written + 2U > ESP_AT_CMD_MAX) {
        return esp_at_mqtt_publish_raw(link_id, topic, data, len,
                                       qos, retain, timeout_ms);
    }
    return esp_at_client_send_sync(line, &r, timeout_ms);
}

    // MQTT 长负载发布（MQTTPUBRAW）。
esp_at_err_t esp_at_mqtt_publish_raw(uint8_t link_id, const char *topic,
                                     const uint8_t *data, uint16_t len,
                                     uint8_t qos, uint8_t retain,
                                     uint32_t timeout_ms)
{
    if (!topic || !data || len == 0) return ESP_AT_ERR_INVALID_ARG;
    char escaped_topic[160];
    if (!mqtt_escape_text(escaped_topic, sizeof escaped_topic,
                          (const uint8_t *)topic, strlen(topic))) {
        return ESP_AT_ERR_INVALID_ARG;
    }

    char line[224];
    const int written = snprintf(line, sizeof line,
             "AT+MQTTPUBRAW=%u,\"%s\",%u,%u,%u",
             link_id, escaped_topic, (unsigned)len, qos, retain);
    if (written <= 0 || written >= (int)sizeof line) {
        return ESP_AT_ERR_INVALID_ARG;
    }

    esp_at_client_t *client = esp_at_client_get();
    if (!client->cmd_mutex
        || xSemaphoreTake(client->cmd_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_AT_ERR_BUSY;
    }
    client->data_prompt_seen = false;
    client->mqtt_pub_result = 0;
    esp_at_err_t result = esp_at_cmd_send_only(line, 1000U);
    if (result != ESP_AT_OK) goto done;
    if (!mqtt_wait_data_prompt(timeout_ms)) {
        result = ESP_AT_ERR_TIMEOUT;
        goto done;
    }

    extern esp_at_err_t esp_at_port_uart_transmit(const uint8_t *payload,
                                                   uint16_t size,
                                                   uint32_t wait_ms);
    result = esp_at_port_uart_transmit(data, len, timeout_ms);
    if (result != ESP_AT_OK) goto done;

    const uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < timeout_ms) {
        if (client->mqtt_pub_result > 0) {
            result = ESP_AT_OK;
            goto done;
        }
        if (client->mqtt_pub_result < 0) {
            result = ESP_AT_ERR_RESP;
            goto done;
        }
        osDelay(1U);
    }
    result = ESP_AT_ERR_TIMEOUT;

done:
    xSemaphoreGive(client->cmd_mutex);
    return result;
}

    // MQTT 订阅（下行通过 ESP_AT_EVENT_MQTT_MESSAGE 回调）。
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

    // MQTT 取消订阅。
esp_at_err_t esp_at_mqtt_unsubscribe(uint8_t link_id, const char *topic, uint32_t timeout_ms)
{
    char line[160];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line, "AT+MQTTUNSUB=%u,\"%s\"", link_id, topic);
    return esp_at_client_send_sync(line, &r, timeout_ms);
}

    // MQTT 断开（MQTTCLEAN）。
esp_at_err_t esp_at_mqtt_disconnect(uint8_t link_id)
{
    char line[24];
    at_cmd_response_t r = {0};
    snprintf(line, sizeof line, "AT+MQTTCLEAN=%u", link_id);
    esp_at_err_t result = esp_at_client_send_sync(line, &r, 5000);
    if (result == ESP_AT_OK) {
        esp_at_client_get()->mqtt_connected = false;
    }
    return result;
}

    // 查询 MQTT 连接状态。
bool esp_at_mqtt_is_connected(void)
{
    return esp_at_client_get()->mqtt_connected;
}
