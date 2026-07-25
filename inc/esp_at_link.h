/**
 * @file    esp_at_link.h
 * @brief   链路抽象接口（UART/SPI 可替换：注册不同 ops）
 */

#ifndef ESP_AT_LINK_H
#define ESP_AT_LINK_H

#include <stdint.h>
#include <stdbool.h>

typedef struct esp_at_link esp_at_link_t;

typedef struct {
    int       (*write)(esp_at_link_t *self, const uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    int       (*read) (esp_at_link_t *self, uint8_t *buf, uint16_t len, uint32_t timeout_ms);
    int       (*peek) (esp_at_link_t *self, uint8_t *dst, uint16_t len);    // 非破坏读
    uint16_t  (*available)(esp_at_link_t *self);                            // 队列字节数
    uint32_t  (*millis)(esp_at_link_t *self);                               // 单调时钟 ms
    void      (*enter_critical)(esp_at_link_t *self);
    void      (*exit_critical) (esp_at_link_t *self);
} esp_at_link_ops_t;

struct esp_at_link {
    const esp_at_link_ops_t *ops;
    void                    *ctx;
};

#define ESP_AT_LINK_WRITE(l, b, n, t)   ((l)->ops->write((l), (b), (n), (t)))
#define ESP_AT_LINK_READ(l, b, n, t)    ((l)->ops->read((l), (b), (n), (t)))
#define ESP_AT_LINK_PEEK(l, b, n)       ((l)->ops->peek((l), (b), (n)))
#define ESP_AT_LINK_AVAILABLE(l)        ((l)->ops->available(l))
#define ESP_AT_LINK_MILLIS(l)           ((l)->ops->millis(l))
#define ESP_AT_LINK_ENTER_CRITICAL(l)   ((l)->ops->enter_critical(l))
#define ESP_AT_LINK_EXIT_CRITICAL(l)    ((l)->ops->exit_critical(l))

// STM32 HAL UART DMA 默认实现
extern const esp_at_link_ops_t esp_at_uart_link_ops;

/**
 * @brief 创建 UART link 实例
 *
 * @param huart    USART 句柄
 * @param hdma_rx  RX DMA 句柄
 * @param hdma_tx  TX DMA 句柄
 * @return esp_at_link_t*
 */
esp_at_link_t *esp_at_uart_link_create(void *huart, void *hdma_rx, void *hdma_tx);

#endif /* ESP_AT_LINK_H */