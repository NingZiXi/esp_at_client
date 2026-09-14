/**
 * @file    esp_at_port_stm32.c
 * @brief   USART2 + DMA1 + IDLE 中断适配（USART2_IRQHandler 由 stm32f4xx_it.c 转发）
 */

#include "esp_at_port_stm32.h"
#include "esp_at_internal.h"
#include "ringbuffer.h"
#include "esp_at_config_default.h"

#include "stm32f4xx_hal.h"
#include "stm_log.h"

#include <string.h>

static UART_HandleTypeDef *s_huart;
static DMA_HandleTypeDef  *s_hdma_rx;
static DMA_HandleTypeDef  *s_hdma_tx;
static volatile bool       s_uart_started;

static uint8_t  s_rx_dma_buf[ESP_AT_UART_RX_BUF_SZ];

// DMA TX，并阻塞等待传输完成。
esp_at_err_t esp_at_port_uart_transmit(const uint8_t *data, uint16_t size, uint32_t timeout_ms)
{
    if (!s_uart_started || !s_huart || !data || size == 0) return ESP_AT_ERR_NOT_READY;
    HAL_StatusTypeDef st = HAL_UART_Transmit_DMA(s_huart, (uint8_t *)data, size);
    if (st != HAL_OK) return ESP_AT_ERR_FAIL;

    uint32_t deadline = HAL_GetTick() + timeout_ms;
    while (__HAL_UART_GET_FLAG(s_huart, UART_FLAG_TC) == RESET) {
        if (HAL_GetTick() >= deadline) {
            (void)HAL_UART_AbortTransmit(s_huart);
            return ESP_AT_ERR_TIMEOUT;
        }
    }
    return ESP_AT_OK;
}

// 启动 UART DMA 和 IDLE 中断。
esp_at_err_t esp_at_port_uart_start(const esp_at_port_config_t *cfg)
{
    if (!cfg || !cfg->huart) return ESP_AT_ERR_INVALID_ARG;
    if (s_uart_started) return ESP_AT_ERR_BUSY;
    s_huart   = cfg->huart;
    s_hdma_rx = cfg->hdma_rx;
    s_hdma_tx = cfg->hdma_tx;

    uint32_t brr = s_huart->Instance->BRR;             // 实际生效波特率
    uint32_t apb1_clk = HAL_RCC_GetPCLK1Freq();
    uint32_t actual_baud = (brr > 0) ? (apb1_clk / brr) : 0;

    LOGI("port", "USART2 cfg.baud=%lu, BRR=%lu, APB1=%lu Hz, actual=%lu",
          cfg->baud, brr, apb1_clk, actual_baud);
    LOGI("port", "hdma_rx=%p, hdma_tx=%p", (void *)s_hdma_rx, (void *)s_hdma_tx);

    if (s_hdma_rx) {
        HAL_StatusTypeDef st = HAL_UARTEx_ReceiveToIdle_DMA(s_huart, s_rx_dma_buf, sizeof s_rx_dma_buf);
        LOGI("port", "ReceiveToIdle_DMA -> %d", (int)st);
        if (st != HAL_OK) {
            s_huart = NULL;
            s_hdma_rx = NULL;
            s_hdma_tx = NULL;
            return ESP_AT_ERR_FAIL;
        }
        __HAL_DMA_DISABLE_IT(s_hdma_rx, DMA_IT_HT);
    } else {
        HAL_StatusTypeDef st = HAL_UARTEx_ReceiveToIdle_IT(s_huart, s_rx_dma_buf, sizeof s_rx_dma_buf);
        LOGI("port", "ReceiveToIdle_IT -> %d", (int)st);
        if (st != HAL_OK) {
            s_huart = NULL;
            s_hdma_rx = NULL;
            s_hdma_tx = NULL;
            return ESP_AT_ERR_FAIL;
        }
    }
    s_uart_started = true;
    return ESP_AT_OK;
}

void esp_at_port_uart_stop(void)
{
    UART_HandleTypeDef *huart = s_huart;
    /* 先禁止回调逻辑，再停止 HAL，避免回调触碰即将释放的 ringbuffer。 */
    s_uart_started = false;
    if (huart) {
        if (s_hdma_rx || s_hdma_tx) {
            (void)HAL_UART_DMAStop(huart);
        } else {
            (void)HAL_UART_AbortReceive_IT(huart);
            (void)HAL_UART_AbortTransmit_IT(huart);
        }
    }
    s_huart = NULL;
    s_hdma_rx = NULL;
    s_hdma_tx = NULL;
}

// 转发 USART2 中断入口（HAL_UARTEx_RxEventCallback 从这里进入）。
void esp_at_port_uart_irq_handler(UART_HandleTypeDef *huart)
{
    if (s_uart_started && huart && s_huart && huart->Instance == s_huart->Instance) {
        HAL_UART_IRQHandler(huart);
    }
}

// IDLE / 缓冲区填满回调：写入 rx_rb、唤醒 rx_task 并重启下一段接收。
void esp_at_port_uart_rx_event(UART_HandleTypeDef *huart, uint16_t size)
{
    if (!s_uart_started || !s_huart || huart != s_huart || size == 0) return;

    uint16_t written = ringbuffer_write(&g_esp_at_client.rx_rb, s_rx_dma_buf, size);
    if (written != size) {
        LOGW("port", "RX ringbuffer full: wrote=%u/%u",
             (unsigned)written, (unsigned)size);
    }

    esp_at_client_notify_rx();
    if (s_uart_started && s_hdma_rx) {
        HAL_StatusTypeDef st = HAL_UARTEx_ReceiveToIdle_DMA(
            s_huart, s_rx_dma_buf, sizeof s_rx_dma_buf);
        if (st == HAL_OK) {
            __HAL_DMA_DISABLE_IT(s_hdma_rx, DMA_IT_HT);
        } else {
            LOGW("port", "restart ReceiveToIdle_DMA failed: %d", (int)st);
        }
    }
}

// TX 完成回调：通知 tx_task。
void esp_at_port_uart_tx_cplt(UART_HandleTypeDef *huart)
{
    if (s_uart_started && huart && huart == s_huart) {
        esp_at_client_notify_tx();
    }
}

// HAL 默认 RxEvent 回调转发
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    esp_at_port_uart_rx_event(huart, Size);
}

// HAL 默认 TxCplt 回调转发
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    esp_at_port_uart_tx_cplt(huart);
}

// 调试：将 rx_rb 的全部字节转储到 RTT。
void esp_at_port_uart_rx_dump(void)
{
    extern esp_at_client_t g_esp_at_client;
    ringbuffer_t *rb = &g_esp_at_client.rx_rb;
    uint16_t avail = ringbuffer_available(rb);
    if (avail == 0) return;

    uint8_t buf[256];
    uint16_t n = (avail > sizeof buf) ? sizeof buf : avail;
    ringbuffer_peek(rb, buf, n);
    char hex[256 * 3 + 1];
    char asc[257];
    uint16_t hx = 0, ac = 0;
    for (uint16_t i = 0; i < n; i++) {
        static const char H[] = "0123456789ABCDEF";
        hex[hx++] = H[buf[i] >> 4];
        hex[hx++] = H[buf[i] & 0x0F];
        hex[hx++] = ' ';
        asc[ac++] = (buf[i] >= 0x20 && buf[i] < 0x7F) ? (char)buf[i] : '.';
    }
    hex[hx] = '\0';
    asc[ac] = '\0';
    LOGD(ESP_AT_PROTO_TAG, ">> hex[%u]: %s", avail, hex);
    LOGD(ESP_AT_PROTO_TAG, ">> asc[%u]: %s", avail, asc);
}

// HAL 同步发送：调度器异常时的发送兜底（HAL_Delay 不依赖 FreeRTOS tick）。
esp_at_port_rc_t esp_at_port_uart_send_and_wait(const char *cmd_line,
                                                 uint32_t wait_ms,
                                                 char *out_buf,
                                                 uint16_t out_buf_sz)
{
    if (!s_uart_started || !s_huart || !cmd_line || !out_buf || out_buf_sz < 8) {
        return ESP_AT_PORT_RC_INVALID;
    }

    extern esp_at_client_t g_esp_at_client;
    ringbuffer_t *rb = &g_esp_at_client.rx_rb;

    ringbuffer_discard(rb, ringbuffer_available(rb));  // 清残留

    size_t raw_len = strlen(cmd_line);
    if (raw_len + 2U > ESP_AT_CMD_MAX) return ESP_AT_PORT_RC_INVALID;
    uint16_t cmd_len = (uint16_t)raw_len;
    uint8_t tx_buf[ESP_AT_CMD_MAX];
    memcpy(tx_buf, cmd_line, cmd_len);
    tx_buf[cmd_len]     = '\r';
    tx_buf[cmd_len + 1] = '\n';

    LOGV(ESP_AT_PROTO_TAG, "<< %s", cmd_line);
    HAL_StatusTypeDef st = HAL_UART_Transmit(s_huart, tx_buf, cmd_len + 2,
                                             (uint16_t)(wait_ms ? wait_ms : 1000));
    if (st != HAL_OK) {
        LOGE("hal_at", "HAL_UART_Transmit failed: %d", (int)st);
        return ESP_AT_PORT_RC_INVALID;
    }

    uint32_t deadline = HAL_GetTick() + wait_ms;
    uint16_t cap = (uint16_t)(out_buf_sz - 1);
    uint16_t pos = 0;
    char last_line[256];
    uint16_t line_len = 0;
    esp_at_port_rc_t rc = ESP_AT_PORT_RC_TIMEOUT;
    bool busy_seen = false;

    while (HAL_GetTick() < deadline) {
        while (pos < cap - 1) {
            uint8_t b;
            uint16_t got = ringbuffer_read(rb, &b, 1);
            if (got == 0) break;
            out_buf[pos++] = (char)b;

            if (b == '\n') {
                last_line[line_len] = '\0';
                if (line_len == 2 && last_line[0] == 'O' && last_line[1] == 'K') {     // OK
                    rc = ESP_AT_PORT_RC_OK;
                    goto done;
                }
                if (line_len == 5 && memcmp(last_line, "ERROR", 5) == 0) {             // ERROR
                    rc = ESP_AT_PORT_RC_ERROR;
                    goto done;
                }
                if (line_len >= 6 && memcmp(last_line, "busy p", 6) == 0) {          // busy p...：让发送端看到后重试
                    busy_seen = true;
                }
                line_len = 0;
            } else if (b != '\r') {
                if (line_len < sizeof last_line - 1) {
                    last_line[line_len++] = (char)b;
                }
            }
        }
        if (rc != ESP_AT_PORT_RC_TIMEOUT) break;
        __NOP();
    }

done:
    out_buf[pos] = '\0';
    if (busy_seen && rc == ESP_AT_PORT_RC_TIMEOUT) {
        rc = ESP_AT_PORT_RC_ERROR;
    }
    if (rc == ESP_AT_PORT_RC_TIMEOUT && pos == 0) {
        LOGW("hal_at", "TIMEOUT with 0 bytes — dump rx_rb:");
        esp_at_port_uart_rx_dump();
    }
    LOGD(ESP_AT_PROTO_TAG, "<< rc=%d, resp=%u bytes", (int)rc, (unsigned)pos);
    if (pos > 0 && pos < 200) {
        LOGD(ESP_AT_PROTO_TAG, ">> %s", out_buf);
    } else if (pos >= 200) {                              // 长响应：截取前 64 字节。
        char head[200];
        uint16_t n = (pos > 64) ? 64 : pos;
        for (uint16_t i = 0; i < n; i++) {
            head[i] = out_buf[i];
        }
        head[n] = '\0';
        LOGD(ESP_AT_PROTO_TAG, ">> resp-head[%u]: %s", pos, head);
    }
    return rc;
}
