/**
 * @file    esp_at_link.c
 * @brief   基于 STM32 HAL UART DMA 的 link ops 实现（不直接动 DMA，由 esp_at_port_stm32 提供）
 */

#include "esp_at_link.h"
#include "esp_at_internal.h"  // g_esp_at_client.rx_rb
#include "ringbuffer.h"

#include <stddef.h>
#include "stm_log.h"

typedef struct {
    void *huart;
    void *hdma_rx;
    void *hdma_tx;
} uart_link_ctx_t;

// 单调毫秒时钟（HAL_GetTick）。
static uint32_t uart_link_millis(esp_at_link_t *self)
{
    (void)self;
    extern uint32_t HAL_GetTick(void);
    return HAL_GetTick();
}

// 进入临界（vTaskSuspendAll）
static void uart_link_enter(esp_at_link_t *self)
{
    (void)self;
    extern void vTaskSuspendAll(void);
    vTaskSuspendAll();
}

// 退出临界（xTaskResumeAll）
static void uart_link_exit(esp_at_link_t *self)
{
    (void)self;
    extern BaseType_t xTaskResumeAll(void);
    (void)xTaskResumeAll();
}

// 队列字节数（rx_rb available）
static uint16_t uart_link_available(esp_at_link_t *self)
{
    (void)self;
    return ringbuffer_available(&g_esp_at_client.rx_rb);
}

// 从 rx_rb 阻塞读 len 字节（超时返回已读字节数）
static int uart_link_read(esp_at_link_t *self, uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)self;
    uint32_t deadline = uart_link_millis(self) + timeout_ms;
    uint16_t got = 0;
    while (got < len) {
        uint16_t n = ringbuffer_read(&g_esp_at_client.rx_rb, buf + got, (uint16_t)(len - got));
        got = (uint16_t)(got + n);
        if (got >= len) break;
        if (uart_link_millis(self) >= deadline) break;
        extern void osDelay(uint32_t);
        osDelay(1);
    }
    return (int)got;
}

// 非破坏读 rx_rb
static int uart_link_peek(esp_at_link_t *self, uint8_t *dst, uint16_t len)
{
    (void)self;
    return (int)ringbuffer_peek(&g_esp_at_client.rx_rb, dst, len);
}

// DMA TX（通过 esp_at_port_uart_transmit 发送）。
static int uart_link_write(esp_at_link_t *self, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)self;
    extern esp_at_err_t esp_at_port_uart_transmit(const uint8_t *data, uint16_t size, uint32_t timeout_ms);
    esp_at_err_t r = esp_at_port_uart_transmit(buf, len, timeout_ms);
    return (r == ESP_AT_OK) ? (int)len : -1;
}

const esp_at_link_ops_t esp_at_uart_link_ops = {
    .write           = uart_link_write,
    .read            = uart_link_read,
    .peek            = uart_link_peek,
    .available       = uart_link_available,
    .millis          = uart_link_millis,
    .enter_critical  = uart_link_enter,
    .exit_critical   = uart_link_exit,
};

// 创建 UART 链路实例。
esp_at_link_t *esp_at_uart_link_create(void *huart, void *hdma_rx, void *hdma_tx)
{
    static uart_link_ctx_t ctx;
    static esp_at_link_t link;
    ctx.huart   = huart;
    ctx.hdma_rx = hdma_rx;
    ctx.hdma_tx = hdma_tx;
    link.ops = &esp_at_uart_link_ops;
    link.ctx = &ctx;
    return &link;
}
