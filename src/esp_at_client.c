/**
 * @file    esp_at_client.c
 * @brief   ESP-AT 客户端对外入口：init / deinit / event cb / sync send
 */

#include "esp_at_client.h"
#include "esp_at_internal.h"
#include "esp_at_link.h"
#include "esp_at_port_stm32.h"

#include <string.h>

#include "stm_log.h"

#define ESP_AT_INIT_TAG "esp_at_init"

#if ESP_AT_DEBUG_LOG
#define ESP_AT_INIT_DBG(fmt, ...)  LOGI(ESP_AT_INIT_TAG, fmt, ##__VA_ARGS__)
#else
#define ESP_AT_INIT_DBG(...)       do {} while (0)
#endif

static const esp_at_port_config_t *s_port_cfg;

// 初始化 ESP-AT 客户端
esp_at_err_t esp_at_init(const esp_at_port_config_t *port_cfg)
{
    if (!port_cfg) return ESP_AT_ERR_INVALID_ARG;
    s_port_cfg = port_cfg;

    esp_at_link_t *link = esp_at_uart_link_create(                 // 创建 link（UART）
        port_cfg->huart, port_cfg->hdma_rx, port_cfg->hdma_tx);

    esp_at_err_t e = esp_at_client_init(link);                     // 分配 ringbuffer / 队列 / 信号量
    if (e != ESP_AT_OK) {
        LOGE(ESP_AT_INIT_TAG, "client_init failed (%d)", e);
        return e;
    }

    e = esp_at_port_uart_start(port_cfg);                          // 启动 UART DMA + IDLE
    if (e != ESP_AT_OK) {
        LOGE(ESP_AT_INIT_TAG, "uart_start failed (%d)", e);
        return e;
    }

    {
        const char *rst = "AT+RST\r\n";                            // 软件复位：清 MQTT 残留 / WiFi 卡死
        extern esp_at_err_t esp_at_port_uart_transmit(const uint8_t *data, uint16_t size, uint32_t timeout_ms);
        esp_at_port_uart_transmit((const uint8_t *)rst, 8, 1000);
        ESP_AT_INIT_DBG("AT+RST sent, waiting 5s for boot");
        HAL_Delay(5000);                                            // 等 boot + 自动重连 WiFi
        ringbuffer_discard(&g_esp_at_client.rx_rb,                 // 清 boot 期间的杂数据
                           ringbuffer_available(&g_esp_at_client.rx_rb));
    }

    esp_at_esp_port_gpio_init(port_cfg);                           // 控制脚
    // rx_task/tx_task/evt_task 延后到 AT 探查成功后再启，否则与 HAL 同步 send_and_wait 抢 rx_rb

    {
        // 5 次 HAL 同步 AT 探查：失败 → hard_reset → 再 5 次
        char probe_buf[64];
        bool probe_ok = false;
        for (int i = 0; i < 5 && !probe_ok; i++) {
            esp_at_port_rc_t rc = esp_at_port_uart_send_and_wait(
                "AT", 1500, probe_buf, sizeof probe_buf);
            if (rc == ESP_AT_PORT_RC_OK) {
                ESP_AT_INIT_DBG("AT probe OK (try %d, resp=%s)", i + 1, probe_buf);
                probe_ok = true;
            } else {
                LOGW(ESP_AT_INIT_TAG, "AT probe failed (%d) try %d/5", (int)rc, i + 1);
                HAL_Delay(500);
            }
        }
        // 5 次全失败 → ESP32 大概率软死锁；如果 en_port 已配置 → 硬复位 + 再轮询
        if (!probe_ok && s_port_cfg->en_port) {
            LOGW(ESP_AT_INIT_TAG, "probes failed → hard reset via EN");
            esp_at_esp_port_hard_reset(s_port_cfg, 8000);
            for (int i = 0; i < 5 && !probe_ok; i++) {
                esp_at_port_rc_t rc = esp_at_port_uart_send_and_wait(
                    "AT", 1500, probe_buf, sizeof probe_buf);
                if (rc == ESP_AT_PORT_RC_OK) {
                    LOGI(ESP_AT_INIT_TAG, "AT probe OK after hard_reset (try %d)", i + 1);
                    probe_ok = true;
                } else {
                    LOGW(ESP_AT_INIT_TAG, "post-reset probe failed (%d) try %d/5", (int)rc, i + 1);
                    HAL_Delay(500);
                }
            }
        }
    }

    // ATE0 关回显：HAL 同步发送（rx_task 还没启）
    {
        char ate_buf[64];
        esp_at_port_rc_t ae = esp_at_port_uart_send_and_wait(
            "ATE0", 1500, ate_buf, sizeof ate_buf);
        if (ae == ESP_AT_PORT_RC_OK) {
            ESP_AT_INIT_DBG("echo disabled (ATE0)");
        } else {
            LOGW(ESP_AT_INIT_TAG, "ATE0 failed (%d); continuing anyway", (int)ae);
        }
    }

    // 探查 + ATE0 全部 HAL 同步后才启 task，ringbuffer 干净
    esp_at_client_start_tasks();                                    // rx/tx/evt 任务

    LOGI(ESP_AT_INIT_TAG, "esp_at_init done");
    return ESP_AT_OK;
}

// 反初始化 ESP-AT 客户端
esp_at_err_t esp_at_deinit(void)
{
    esp_at_client_deinit();
    esp_at_esp_port_power_off(s_port_cfg);
    return ESP_AT_OK;
}

// 同步等待 ready URC
uint32_t esp_at_wait_ready(uint32_t timeout_ms)
{
    return esp_at_client_wait_ready(timeout_ms);
}

// 注册事件回调（evt==ESP_AT_EVENT_ANY 走通配）
esp_at_err_t esp_at_register_event_cb(esp_at_event_t evt, esp_at_event_cb_t cb, void *user)
{
    esp_at_client_t *c = esp_at_client_get();
    if (evt == ESP_AT_EVENT_ANY) {                                 // -1 走 cbs_any，不能做 cbs[] 下标
        c->cbs_any     = cb;
        c->cb_user_any = user;
        return ESP_AT_OK;
    }
    if (evt < 0 || evt >= ESP_AT_EVENT_MAX) return ESP_AT_ERR_INVALID_ARG;
    c->cbs[evt]     = cb;
    c->cb_user[evt] = user;
    return ESP_AT_OK;
}

// 反注册事件回调
esp_at_err_t esp_at_unregister_event_cb(esp_at_event_t evt, esp_at_event_cb_t cb)
{
    esp_at_client_t *c = esp_at_client_get();
    if (evt == ESP_AT_EVENT_ANY) {
        if (c->cbs_any != cb) return ESP_AT_ERR_INVALID_ARG;
        c->cbs_any     = NULL;
        c->cb_user_any = NULL;
        return ESP_AT_OK;
    }
    if (evt < 0 || evt >= ESP_AT_EVENT_MAX) return ESP_AT_ERR_INVALID_ARG;
    if (c->cbs[evt] != cb) return ESP_AT_ERR_INVALID_ARG;
    c->cbs[evt]     = NULL;
    c->cb_user[evt] = NULL;
    return ESP_AT_OK;
}

// 同步发送 AT 命令并等待响应
esp_at_err_t esp_at_cmd_send_sync(const char *cmd_line, at_cmd_response_t *resp, uint32_t timeout_ms)
{
    if (!cmd_line || !resp) return ESP_AT_ERR_INVALID_ARG;
    if (timeout_ms == 0) timeout_ms = ESP_AT_CMD_TIMEOUT_DEFAULT_MS;
    return esp_at_client_send_sync(cmd_line, resp, timeout_ms);
}

// 仅发送 AT 命令不等响应（探测用）
esp_at_err_t esp_at_cmd_send_only(const char *cmd_line, uint32_t timeout_ms)
{
    if (!cmd_line) return ESP_AT_ERR_INVALID_ARG;
    esp_at_client_t *c = esp_at_client_get();
    if (!c->inited) return ESP_AT_ERR_NOT_READY;

    uint16_t cmd_len = (uint16_t)strlen(cmd_line);
    uint16_t total = (uint16_t)(cmd_len + 2);
    uint8_t *buf = (uint8_t *)pvPortMalloc(total);
    if (!buf) return ESP_AT_ERR_NO_MEM;
    memcpy(buf, cmd_line, cmd_len);
    buf[cmd_len]     = '\r';
    buf[cmd_len + 1] = '\n';
    int rc = ESP_AT_LINK_WRITE(c->link, buf, total, timeout_ms);
    vPortFree(buf);
    return (rc < 0) ? ESP_AT_ERR_FAIL : ESP_AT_OK;
}

// 读 ESP-AT 固件版本（AT+GMR）
esp_at_err_t esp_at_client_get_version(char *out, uint16_t out_sz, uint32_t timeout_ms)
{
    if (!out || out_sz < 16) return ESP_AT_ERR_INVALID_ARG;
    at_cmd_response_t r = {0};
    esp_at_err_t e = esp_at_cmd_send_sync("AT+GMR", &r, timeout_ms);
    if (e != ESP_AT_OK) return e;
    strncpy(out, r.text, out_sz - 1);
    out[out_sz - 1] = '\0';
    return ESP_AT_OK;
}