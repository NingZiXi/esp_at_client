/**
 * @file    esp_at_internal.c
 * @brief   AT Client 行解析 + 状态机 + 同步发送
 */

#include "esp_at_internal.h"
#include "esp_at_config_default.h"
#include "ringbuffer.h"

#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"
#include "esp_at_port_stm32.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#include "stm_log.h"

#define ESP_AT_LOG_TAG  "at_client"
#if ESP_AT_LOG_LEVEL >= 5
#define ESP_AT_LOGV(fmt, ...) LOGV(ESP_AT_LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define ESP_AT_LOGV(...) do {} while (0)
#endif
#if ESP_AT_LOG_LEVEL >= 4
#define ESP_AT_LOGD(fmt, ...) LOGD(ESP_AT_LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define ESP_AT_LOGD(...) do {} while (0)
#endif
#if ESP_AT_LOG_LEVEL >= 3
#define ESP_AT_LOGI(fmt, ...) LOGI(ESP_AT_LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define ESP_AT_LOGI(...) do {} while (0)
#endif
#if ESP_AT_LOG_LEVEL >= 2
#define ESP_AT_LOGW(fmt, ...) LOGW(ESP_AT_LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define ESP_AT_LOGW(...) do {} while (0)
#endif
#if ESP_AT_LOG_LEVEL >= 1
#define ESP_AT_LOGE(fmt, ...) LOGE(ESP_AT_LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define ESP_AT_LOGE(...) do {} while (0)
#endif

esp_at_client_t g_esp_at_client;

// 单例获取
esp_at_client_t *esp_at_client_get(void)
{
    return &g_esp_at_client;
}

// 行匹配：检查 line 是否以 prefix 开头
bool esp_at_match_prefix(const char *line, const char *prefix)
{
    if (!line || !prefix) return false;
    return strncmp(line, prefix, strlen(prefix)) == 0;
}

// 行内解析整数：找 key 后整数（key 含或不含 =/:）
bool esp_at_extract_int(const char *line, const char *key, int *out)
{
    if (!line || !key || !out) return false;
    const char *p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    if (*p == '=' || *p == ':') p++;
    *out = (int)strtol(p, NULL, 10);
    return true;
}

// 行内解析字符串：找 "key":"..."
bool esp_at_extract_str(const char *line, const char *key, char *out, uint16_t out_sz)
{
    if (!line || !key || !out || out_sz == 0) return false;
    const char *p = strstr(line, key);
    if (!p) return false;
    p += strlen(key);
    if (*p == '=' || *p == ':') p++;
    if (*p != '"') return false;
    p++;
    uint16_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

// DMA RX 完成回调中调用：通知 rx_task
void esp_at_client_notify_rx(void)
{
    BaseType_t hp = pdFALSE;
    if (g_esp_at_client.rx_task_h) {
        vTaskNotifyGiveFromISR(g_esp_at_client.rx_task_h, &hp);
    }
    portYIELD_FROM_ISR(hp);
}

// DMA TX 完成回调中调用：通知 tx_task
void esp_at_client_notify_tx(void)
{
    BaseType_t hp = pdFALSE;
    if (g_esp_at_client.tx_task_h) {
        vTaskNotifyGiveFromISR(g_esp_at_client.tx_task_h, &hp);
    }
    portYIELD_FROM_ISR(hp);
}

// 设置 ready 位
void esp_at_client_set_ready(void)
{
    if (g_esp_at_client.boot_eg) {
        xEventGroupSetBits(g_esp_at_client.boot_eg, BOOT_READY_BIT);
    }
}

// 查询 ready
bool esp_at_client_is_ready(void)
{
    if (!g_esp_at_client.boot_eg) return false;
    EventBits_t b = xEventGroupGetBits(g_esp_at_client.boot_eg);
    return (b & BOOT_READY_BIT) != 0;
}

// 阻塞等 ready
uint32_t esp_at_client_wait_ready(uint32_t timeout_ms)
{
    if (!g_esp_at_client.boot_eg) return timeout_ms;
    uint32_t start = HAL_GetTick();
    EventBits_t b = xEventGroupWaitBits(
        g_esp_at_client.boot_eg, BOOT_READY_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (b & BOOT_READY_BIT) {
        return HAL_GetTick() - start;
    }
    return timeout_ms;
}

// 行解析 URC 转事件投递（异步：推到 urc_queue，evt_task 异步分发）
esp_at_err_t esp_at_client_post_event(esp_at_event_t evt, const esp_at_event_payload_t *payload)
{
    if (!g_esp_at_client.inited) return ESP_AT_ERR_NOT_READY;
    if (evt < 0 || evt >= ESP_AT_EVENT_MAX) return ESP_AT_ERR_INVALID_ARG;

    // 复制 payload 到堆（evt_task 异步消费，原 stack 帧不能复用）
    esp_at_event_payload_t *p = (esp_at_event_payload_t *)pvPortMalloc(sizeof *p);
    if (!p) return ESP_AT_ERR_NO_MEM;
    *p = payload ? *payload : (esp_at_event_payload_t){0};
    p->type = evt;

    // 深拷贝 topic / data：原指针指向 rx_task 行缓冲区，evt_task 派发时已被覆盖
    if (payload && payload->topic && payload->topic_len > 0) {
        char *topic_copy = (char *)pvPortMalloc(payload->topic_len + 1);
        if (topic_copy) {
            memcpy(topic_copy, payload->topic, payload->topic_len);
            topic_copy[payload->topic_len] = '\0';
            p->topic = (const char *)topic_copy;
        } else {
            p->topic = NULL;
            p->topic_len = 0;
        }
    }
    if (payload && payload->data && payload->data_len > 0) {
        uint8_t *data_copy = (uint8_t *)pvPortMalloc(payload->data_len);
        if (data_copy) {
            memcpy(data_copy, payload->data, payload->data_len);
            p->data = data_copy;
        } else {
            p->data = NULL;
            p->data_len = 0;
        }
    }

    if (xQueueSend(g_esp_at_client.urc_queue, &p, 0) != pdTRUE) {
        if (p->topic) vPortFree((void *)p->topic);
        if (p->data)  vPortFree((void *)p->data);
        vPortFree(p);
        return ESP_AT_ERR_FAIL;
    }
    return ESP_AT_OK;
}

// 去掉行尾 \r\n 和尾随空格
static void trim_cr(char *line, uint16_t *len)
{
    while (*len > 0 && (line[*len - 1] == '\r' || line[*len - 1] == '\n' ||
                        line[*len - 1] == ' ')) {
        line[--(*len)] = '\0';
    }
}

// 处理 "ready" 行：标记 ready + 派发 ESP_AT_EVENT_READY
static void handle_line_ready(const char *line)
{
    ESP_AT_LOGI("ready");
    g_esp_at_client.wifi_state = ESP_AT_WIFI_IDLE;
    esp_at_client_set_ready();
    esp_at_client_post_event(ESP_AT_EVENT_READY, NULL);
}

// 处理 WIFI CONNECTED / WIFI GOT IP / WIFI DISCONNECT
static void handle_line_wifi(const char *line, const esp_at_event_payload_t *blank)
{
    if (esp_at_match_prefix(line, "WIFI CONNECTED")) {
        g_esp_at_client.wifi_state = ESP_AT_WIFI_CONNECTED;
        esp_at_client_post_event(ESP_AT_EVENT_WIFI_CONNECTED, blank);
    } else if (esp_at_match_prefix(line, "WIFI GOT IP")) {
        g_esp_at_client.wifi_state = ESP_AT_WIFI_GOT_IP;
        esp_at_client_post_event(ESP_AT_EVENT_WIFI_GOT_IP, blank);
    } else if (esp_at_match_prefix(line, "WIFI DISCONNECT")) {
        g_esp_at_client.wifi_state = ESP_AT_WIFI_LOST;
        esp_at_client_post_event(ESP_AT_EVENT_WIFI_DISCONNECT, blank);
    }
}

// 处理 MQTT URC（CONNECTED/DISCONNECTED/PUB OK/FAIL/SUBRECV/SUB）
static void handle_line_mqtt_urc(const char *line)
{
    esp_at_event_payload_t p = {0};

    if (esp_at_match_prefix(line, "+MQTTCONNECTED")) {
        int link = 0;
        esp_at_extract_int(line, "+MQTTCONNECTED", &link);
        p.link_id = link;
        g_esp_at_client.mqtt_connected = true;
        esp_at_client_post_event(ESP_AT_EVENT_MQTT_CONNECTED, &p);
        return;
    }
    if (esp_at_match_prefix(line, "+MQTTDISCONNECTED")) {
        int link = 0;
        esp_at_extract_int(line, "+MQTTDISCONNECTED", &link);
        p.link_id = link;
        g_esp_at_client.mqtt_connected = false;
        esp_at_client_post_event(ESP_AT_EVENT_MQTT_DISCONNECTED, &p);
        return;
    }
    if (esp_at_match_prefix(line, "+MQTTPUB:OK")) {
        esp_at_client_post_event(ESP_AT_EVENT_MQTT_PUB_OK, &p);
        return;
    }
    if (esp_at_match_prefix(line, "+MQTTPUB:FAIL")) {
        esp_at_client_post_event(ESP_AT_EVENT_MQTT_PUB_FAIL, &p);
        return;
    }
    if (esp_at_match_prefix(line, "+MQTTSUBRECV")) {
        /* +MQTTSUBRECV:<link>,"<topic>",<len>,<data>
         * 数据末尾与 len 之间是倒数第二个逗号——直接 strrchr 会拿到 data 段 */
        int link = 0;
        esp_at_extract_int(line, "+MQTTSUBRECV", &link);
        p.link_id = link;

        const char *q1 = strchr(line, '"');
        const char *q2 = NULL;
        if (q1) {
            q1++;
            q2 = strchr(q1, '"');
            if (q2) {
                p.topic_len = (uint16_t)(q2 - q1);
                p.topic = q1;
            }
        }

        /* strrchr/strtol 必须在写 '\0' 之前完成——改完 q2 后字符串被截断 */
        const char *last = strrchr(line, ',');
        if (last) {
            const char *prev = NULL;
            for (const char *s = line; s < last; s++) {
                if (*s == ',') prev = s;
            }
            if (prev) {
                int len = (int)strtol(prev + 1, NULL, 10);
                p.data_len = (uint16_t)len;
                p.data = (const uint8_t *)(last + 1);
            }
        }

        if (q2) *(char *)q2 = '\0';                 // 让 topic 终止，data 用 %.*s 限长打印
        esp_at_client_post_event(ESP_AT_EVENT_MQTT_MESSAGE, &p);
        return;
    }
    if (esp_at_match_prefix(line, "+MQTTSUB:")) {
        // SUB/UNSUB 返回（OK/FAIL）
    }
}

// 处理 +HTTPCLIENT:<size>,<data> 多帧响应
static void handle_line_http_urc(const char *line)
{
    if (esp_at_match_prefix(line, "+HTTPCLIENT")) {
        // 把整行追加到 pending.resp->text，给 esp_at_http_request 拼装完整 body
        if (g_esp_at_client.pending.resp) {
            at_cmd_response_t *r = g_esp_at_client.pending.resp;
            uint16_t line_len = (uint16_t)strlen(line);
            uint16_t avail = (uint16_t)(AT_RESP_TEXT_MAX - 3 - r->text_len);
            uint16_t copy = (line_len < avail) ? line_len : avail;
            if (copy > 0) {
                memcpy(r->text + r->text_len, line, copy);
                r->text_len = (uint16_t)(r->text_len + copy);
                r->text[r->text_len++] = '\r';
                r->text[r->text_len++] = '\n';
                r->text[r->text_len] = '\0';
            }
        }

        esp_at_event_payload_t p = {0};
        int size = 0;
        esp_at_extract_int(line, "+HTTPCLIENT", &size);
        p.data_len = (uint16_t)size;
        const char *c = strchr(line, ',');
        if (c) {
            p.data = (const uint8_t *)(c + 1);
        }
        esp_at_client_post_event(ESP_AT_EVENT_HTTP_DONE, &p);
    }
}

// 处理 +CWJAP:<errcode> ERROR
static void handle_line_cwjap_err(const char *line)
{
    if (esp_at_match_prefix(line, "+CWJAP:")) {
        int err = 0;
        esp_at_extract_int(line, "+CWJAP", &err);
        esp_at_event_payload_t p = {0};
        p.err_code = (uint32_t)err;
        g_esp_at_client.wifi_state = ESP_AT_WIFI_LOST;
        esp_at_client_post_event(ESP_AT_EVENT_WIFI_DISCONNECT, &p);
    }
}

// 处理 ERR CODE:0x%08x
static void handle_line_err_code(const char *line)
{
    if (esp_at_match_prefix(line, "ERR CODE:")) {
        uint32_t code = 0;
        const char *p = strchr(line, ':');
        if (p) {
            code = strtoul(p + 1, NULL, 16);
        }
        esp_at_event_payload_t pl = {0};
        pl.err_code = code;
        esp_at_client_post_event(ESP_AT_EVENT_ERROR, &pl);
    }
}

// 完成当前 pending 命令：写回 status / elapsed_ms，释放 semaphore
static void finish_pending(at_resp_status_t status)
{
    if (!g_esp_at_client.pending.resp) return;
    if (g_esp_at_client.pending.resp) {
        g_esp_at_client.pending.resp->status = status;
        g_esp_at_client.pending.resp->elapsed_ms =
            HAL_GetTick() - (g_esp_at_client.state_enter_ms);
    }
    g_esp_at_client.pending.resp = NULL;
    g_esp_at_client.state = ESP_AT_STATE_IDLE;
    xSemaphoreGive(g_esp_at_client.cmd_done_sem);
}

// 行解析入口
static void process_line(char *line, uint16_t len)
{
    trim_cr(line, &len);
    if (len == 0) return;

    // +IPD 行 body 字节可能几百，避免 RTT 被淹没：只打印前 30 字节摘要
    if (esp_at_match_prefix(line, "+IPD")) {
        char head[40];
        uint16_t copy = (len < sizeof head - 1) ? len : (uint16_t)(sizeof head - 1);
        memcpy(head, line, copy);
        head[copy] = '\0';
        ESP_AT_LOGD(">> %s... (%u bytes)", head, (unsigned)len);
        return;
    }

    LOGD(ESP_AT_PROTO_TAG, ">> %s", line);

    if (strcmp(line, "ready") == 0) {
        handle_line_ready(line);
        return;
    }
    if (esp_at_match_prefix(line, "WIFI ")) {
        handle_line_wifi(line, NULL);
        return;
    }
    if (esp_at_match_prefix(line, "+MQTT")) {
        handle_line_mqtt_urc(line);                // 部分 URC 同当前命令响应（MQTTCONN OK 后 +MQTTCONNECTED），不 finish
        return;
    }
    if (esp_at_match_prefix(line, "+HTTPCLIENT")) {
        handle_line_http_urc(line);
        return;
    }
    if (esp_at_match_prefix(line, "+CWJAP:")) {
        handle_line_cwjap_err(line);
        return;
    }
    if (esp_at_match_prefix(line, "ERR CODE:")) {
        handle_line_err_code(line);
        return;
    }
    if (esp_at_match_prefix(line, "busy p")) {
        finish_pending(AT_RESP_BUSY);
        return;
    }
    if (line[0] == '>') {                          // DATA_PROMPT → RAW_TX
        g_esp_at_client.state = ESP_AT_STATE_DATA_PROMPT;
        return;
    }

    if (g_esp_at_client.pending.resp) {
        if (strcmp(line, "OK") == 0) {
            finish_pending(AT_RESP_OK);
            return;
        }
        if (strcmp(line, "ERROR") == 0 || strcmp(line, "FAIL") == 0) {
            finish_pending(AT_RESP_ERROR);
            return;
        }
        /* 多行查询响应（CWJAP? / CWMODE? / CIPSTA 等），每行强制 \r\n 分隔 */
        at_cmd_response_t *r = g_esp_at_client.pending.resp;
        uint16_t avail = (uint16_t)(AT_RESP_TEXT_MAX - 3 - r->text_len);
        uint16_t copy = (len < avail) ? len : avail;
        if (copy > 0) {
            memcpy(r->text + r->text_len, line, copy);
            r->text_len = (uint16_t)(r->text_len + copy);
            r->text[r->text_len++] = '\r';
            r->text[r->text_len++] = '\n';
            r->text[r->text_len] = '\0';
        }
    }
}

// 应用层周期调用：从 rx_rb 抽完整行跑 process_line
void esp_at_client_pump_rx(void)
{
    char line[ESP_AT_LINE_MAX];
    while (1) {
        int pos = ringbuffer_find_char(&g_esp_at_client.rx_rb, '\n');
        if (pos < 0) break;
        uint16_t copy = (uint16_t)(pos > ESP_AT_LINE_MAX - 1 ? ESP_AT_LINE_MAX - 1 : pos);
        uint16_t got = ringbuffer_read(&g_esp_at_client.rx_rb, (uint8_t *)line, copy);
        uint8_t drop;
        ringbuffer_read(&g_esp_at_client.rx_rb, &drop, 1);
        line[got] = '\0';
        process_line(line, got);
    }
}

// RX 任务入口（FreeRTOS task arg 未用）
void esp_at_client_rx_task(void *arg)
{
    (void)arg;
    char line[ESP_AT_LINE_MAX];
    ESP_AT_LOGI("rx_task start");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        for (;;) {
            // 反复处理 rx_rb 顶部的 +IPD 帧（一次 notify 可能含多帧）
            bool ipd_handled = false;
            uint8_t peek[4];
            while (1) {
                if (!g_esp_at_client.ipd_active) {
                    if (!ringbuffer_peek(&g_esp_at_client.rx_rb, peek, 4)
                        || memcmp(peek, "+IPD", 4) != 0) {
                        break;
                    }

                    /* 先窥读完整头部；头部跨 DMA 分段时不得提前消费。 */
                    uint16_t avail = ringbuffer_available(&g_esp_at_client.rx_rb);
                    uint16_t cap = (avail < 24U) ? avail : 24U;
                    char hdr[24];
                    uint16_t colon = UINT16_MAX;
                    if (cap > 0) {
                        ringbuffer_peek(&g_esp_at_client.rx_rb,
                                        (uint8_t *)hdr, cap);
                        for (uint16_t i = 0; i < cap; i++) {
                            if (hdr[i] == ':') { colon = i; break; }
                        }
                    }
                    if (colon == UINT16_MAX) {
                        if (avail >= sizeof hdr - 1U) {
                            ESP_AT_LOGW("malformed +IPD header, resync");
                            ringbuffer_discard(&g_esp_at_client.rx_rb, 1);
                            ipd_handled = true;
                            continue;
                        }
                        break; /* 等待下一段 DMA 数据 */
                    }

                    uint16_t hdr_len = (uint16_t)(colon + 1U);
                    ringbuffer_peek(&g_esp_at_client.rx_rb,
                                    (uint8_t *)hdr, hdr_len);
                    hdr[colon] = '\0';
                    const char *last_comma = strrchr(hdr + 4, ',');
                    int frame_len = last_comma ? atoi(last_comma + 1) : 0;
                    ringbuffer_discard(&g_esp_at_client.rx_rb, hdr_len);
                    if (frame_len <= 0 || (unsigned long)frame_len > UINT16_MAX) {
                        ESP_AT_LOGW("invalid +IPD header: %s", hdr);
                        ipd_handled = true;
                        continue;
                    }

                    g_esp_at_client.ipd_active = true;
                    g_esp_at_client.ipd_expected = (uint16_t)frame_len;
                    g_esp_at_client.ipd_received = 0;
                    g_esp_at_client.ipd_drop =
                        ((uint32_t)g_esp_at_client.ipd_len +
                         (uint32_t)frame_len > sizeof g_esp_at_client.ipd_buf);
                    if (g_esp_at_client.ipd_drop) {
                        ESP_AT_LOGW("+IPD overflow: buffered=%u frame=%d",
                                    (unsigned)g_esp_at_client.ipd_len, frame_len);
                    }
                }

                uint16_t available = ringbuffer_available(&g_esp_at_client.rx_rb);
                uint16_t remaining = (uint16_t)(g_esp_at_client.ipd_expected -
                                                g_esp_at_client.ipd_received);
                uint16_t take = (available < remaining) ? available : remaining;
                if (take == 0) break; /* body 尚未收全，等待下一次 notify */

                uint16_t write_off = g_esp_at_client.ipd_len;
                if (!g_esp_at_client.ipd_drop) {
                    ringbuffer_read(&g_esp_at_client.rx_rb,
                                    g_esp_at_client.ipd_buf + write_off, take);
                    g_esp_at_client.ipd_len = (uint16_t)(write_off + take);
                } else {
                    ringbuffer_discard(&g_esp_at_client.rx_rb, take);
                }
                g_esp_at_client.ipd_received = (uint16_t)(g_esp_at_client.ipd_received + take);
                g_esp_at_client.ipd_active =
                    (g_esp_at_client.ipd_received < g_esp_at_client.ipd_expected);
                if (!g_esp_at_client.ipd_active) {
                    g_esp_at_client.ipd_drop = false;
                    g_esp_at_client.ipd_expected = 0;
                    g_esp_at_client.ipd_received = 0;
                }
                ipd_handled = true;
            }
            if (ipd_handled) continue;

            int pos = ringbuffer_find_char(&g_esp_at_client.rx_rb, '\n');
            if (pos < 0) break;
            uint16_t copy = (uint16_t)(pos > ESP_AT_LINE_MAX - 1 ? ESP_AT_LINE_MAX - 1 : pos);
            uint16_t got = ringbuffer_read(&g_esp_at_client.rx_rb, (uint8_t *)line, copy);
            uint8_t drop;
            ringbuffer_read(&g_esp_at_client.rx_rb, &drop, 1);
            line[got] = '\0';
            process_line(line, got);
        }
    }
}

// evt_task：scheduler 正常环境下从此队列取事件派发
static void evt_task_entry(void *arg)
{
    (void)arg;
    esp_at_event_payload_t *p = NULL;
    for (;;) {
        if (xQueueReceive(g_esp_at_client.urc_queue, &p, portMAX_DELAY) != pdTRUE) continue;
        if (!p) continue;
        if (p->type < ESP_AT_EVENT_MAX && g_esp_at_client.cbs[p->type]) {
            g_esp_at_client.cbs[p->type](p, g_esp_at_client.cb_user[p->type]);
        }
        // ESP_AT_EVENT_ANY 通配：每个事件多派一次
        if (g_esp_at_client.cbs_any) {
            g_esp_at_client.cbs_any(p, g_esp_at_client.cb_user_any);
        }
        // 释放 post_event 深拷贝的 topic / data
        if (p->topic) vPortFree((void *)p->topic);
        if (p->data)  vPortFree((void *)p->data);
        vPortFree(p);
    }
}

// tx_task：保留通知接口（实际发送在 send_sync 直调 link.write）
static void tx_task_entry(void *arg)
{
    (void)arg;
    ESP_AT_LOGI("tx_task start");
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

// 同步发送：清 rx_rb / 拼 CRLF / 写 link / 自抽行 / 收 OK|ERROR|busy
esp_at_err_t esp_at_client_send_sync(const char *cmd_line,
                                     at_cmd_response_t *resp,
                                     uint32_t timeout_ms)
{
    if (!g_esp_at_client.inited) return ESP_AT_ERR_NOT_READY;
    if (!cmd_line || !resp) return ESP_AT_ERR_INVALID_ARG;

    if (!g_esp_at_client.cmd_mutex ||
        xSemaphoreTake(g_esp_at_client.cmd_mutex,
                       timeout_ms ? pdMS_TO_TICKS(timeout_ms) : portMAX_DELAY) != pdTRUE) {
        return ESP_AT_ERR_BUSY;
    }

    if (g_esp_at_client.pending.resp) {
        xSemaphoreGive(g_esp_at_client.cmd_mutex);
        return ESP_AT_ERR_BUSY;
    }

    /* 清除上一次超时后迟到响应留下的完成信号，避免误完成新事务。 */
    while (xSemaphoreTake(g_esp_at_client.cmd_done_sem, 0) == pdTRUE) {
        /* drain */
    }

    memset(resp, 0, sizeof *resp);
    g_esp_at_client.pending.resp = resp;
    g_esp_at_client.pending.deadline_ms = HAL_GetTick() + timeout_ms;
    g_esp_at_client.pending.retry_left = ESP_AT_CMD_RETRY_MAX;
    g_esp_at_client.pending.is_data_prompt = false;
    g_esp_at_client.state = ESP_AT_STATE_SENDING;
    g_esp_at_client.state_enter_ms = HAL_GetTick();

    size_t raw_len = strlen(cmd_line);
    if (raw_len + 2U > ESP_AT_CMD_MAX) {
        g_esp_at_client.pending.resp = NULL;
        xSemaphoreGive(g_esp_at_client.cmd_mutex);
        return ESP_AT_ERR_INVALID_ARG;
    }
    uint16_t cmd_len = (uint16_t)raw_len;
    uint16_t total = (uint16_t)(cmd_len + 2U);
    uint8_t buf[ESP_AT_CMD_MAX];
    memcpy(buf, cmd_line, cmd_len);
    buf[cmd_len]     = '\r';
    buf[cmd_len + 1] = '\n';

    LOGV(ESP_AT_PROTO_TAG, "<< %s", cmd_line);
    int rc = ESP_AT_LINK_WRITE(g_esp_at_client.link, buf, total, timeout_ms);
    if (rc < 0) {
        g_esp_at_client.pending.resp = NULL;
        xSemaphoreGive(g_esp_at_client.cmd_mutex);
        return ESP_AT_ERR_FAIL;
    }
    g_esp_at_client.state = ESP_AT_STATE_WAITING;

    /* 等 rx_task 解析完成（finish_pending 释放 cmd_done_sem） */
    TickType_t ticks_to_wait = (timeout_ms == 0) ? portMAX_DELAY
                                                    : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(g_esp_at_client.cmd_done_sem, ticks_to_wait) != pdTRUE) {
        g_esp_at_client.pending.resp = NULL;
        g_esp_at_client.state = ESP_AT_STATE_IDLE;
        xSemaphoreGive(g_esp_at_client.cmd_mutex);
        return ESP_AT_ERR_TIMEOUT;
    }
    esp_at_err_t result = ESP_AT_ERR_RESP;
    if (resp->status == AT_RESP_OK) result = ESP_AT_OK;
    else if (resp->status == AT_RESP_BUSY) result = ESP_AT_ERR_BUSY;
    else if (resp->status == AT_RESP_TIMEOUT) result = ESP_AT_ERR_TIMEOUT;
    xSemaphoreGive(g_esp_at_client.cmd_mutex);
    return result;
}

// 客户端初始化：分配 ringbuffer / 同步原语
esp_at_err_t esp_at_client_init(esp_at_link_t *link)
{
    if (g_esp_at_client.inited) return ESP_AT_OK;
    if (!link) return ESP_AT_ERR_INVALID_ARG;

    memset(&g_esp_at_client, 0, sizeof g_esp_at_client);
    g_esp_at_client.link = link;

    g_esp_at_client.rx_storage =
        (uint8_t *)pvPortMalloc(ESP_AT_RINGBUFFER_SZ);
    if (!g_esp_at_client.rx_storage) return ESP_AT_ERR_NO_MEM;
    ringbuffer_init(&g_esp_at_client.rx_rb,
                    g_esp_at_client.rx_storage, ESP_AT_RINGBUFFER_SZ);

    g_esp_at_client.cmd_done_sem = xSemaphoreCreateBinary();
    if (!g_esp_at_client.cmd_done_sem) goto fail;
    g_esp_at_client.cmd_mutex = xSemaphoreCreateMutex();
    if (!g_esp_at_client.cmd_mutex) goto fail;
    g_esp_at_client.urc_queue = xQueueCreate(8, sizeof(esp_at_event_payload_t *));
    if (!g_esp_at_client.urc_queue) goto fail;
    g_esp_at_client.boot_eg = xEventGroupCreate();
    if (!g_esp_at_client.boot_eg) goto fail;

    g_esp_at_client.inited = true;
    return ESP_AT_OK;

fail:
    if (g_esp_at_client.boot_eg) vEventGroupDelete(g_esp_at_client.boot_eg);
    if (g_esp_at_client.urc_queue) vQueueDelete(g_esp_at_client.urc_queue);
    if (g_esp_at_client.cmd_mutex) vSemaphoreDelete(g_esp_at_client.cmd_mutex);
    if (g_esp_at_client.cmd_done_sem) vSemaphoreDelete(g_esp_at_client.cmd_done_sem);
    if (g_esp_at_client.rx_storage) vPortFree(g_esp_at_client.rx_storage);
    memset(&g_esp_at_client, 0, sizeof g_esp_at_client);
    return ESP_AT_ERR_NO_MEM;
}

// 启动 rx / tx / evt 任务
void esp_at_client_start_tasks(void)
{
    if (!g_esp_at_client.inited) return;

    BaseType_t ok;
    ok = xTaskCreate(esp_at_client_rx_task, "at_rx",
                     ESP_AT_TASK_RX_STACK, NULL,
                     osPriorityAboveNormal, &g_esp_at_client.rx_task_h);
    if (ok != pdPASS) {
        ESP_AT_LOGE("rx task create failed");
    }
    ok = xTaskCreate(tx_task_entry, "at_tx",
                     ESP_AT_TASK_TX_STACK, NULL,
                     osPriorityAboveNormal, &g_esp_at_client.tx_task_h);
    if (ok != pdPASS) {
        ESP_AT_LOGE("tx task create failed");
    }
    ok = xTaskCreate(evt_task_entry, "at_evt",
                     ESP_AT_TASK_EVT_STACK, NULL,
                     osPriorityNormal, &g_esp_at_client.evt_task_h);
    if (ok != pdPASS) {
        ESP_AT_LOGE("evt task create failed");
    }
}

// 反初始化：删任务 / 删同步原语 / 释放内存
esp_at_err_t esp_at_client_deinit(void)
{
    if (!g_esp_at_client.inited) return ESP_AT_OK;
    esp_at_port_uart_stop();
    TaskHandle_t rx_task = g_esp_at_client.rx_task_h;
    TaskHandle_t tx_task = g_esp_at_client.tx_task_h;
    TaskHandle_t evt_task = g_esp_at_client.evt_task_h;
    g_esp_at_client.rx_task_h = NULL;
    g_esp_at_client.tx_task_h = NULL;
    g_esp_at_client.evt_task_h = NULL;
    if (rx_task) vTaskDelete(rx_task);
    if (tx_task) vTaskDelete(tx_task);
    if (evt_task) vTaskDelete(evt_task);
    if (g_esp_at_client.cmd_done_sem) vSemaphoreDelete(g_esp_at_client.cmd_done_sem);
    if (g_esp_at_client.cmd_mutex) vSemaphoreDelete(g_esp_at_client.cmd_mutex);
    if (g_esp_at_client.urc_queue) vQueueDelete(g_esp_at_client.urc_queue);
    if (g_esp_at_client.boot_eg) vEventGroupDelete(g_esp_at_client.boot_eg);
    if (g_esp_at_client.rx_storage) vPortFree(g_esp_at_client.rx_storage);
    if (g_esp_at_client.raw_rx_buf) vPortFree(g_esp_at_client.raw_rx_buf);
    memset(&g_esp_at_client, 0, sizeof g_esp_at_client);
    return ESP_AT_OK;
}
