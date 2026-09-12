/**
 * @file    esp_at_port_stm32.h
 * @brief   STM32 HAL 适配：USART2 + DMA + IDLE 中断 + ESP EN/RST 控制脚
 */

#ifndef ESP_AT_PORT_STM32_H
#define ESP_AT_PORT_STM32_H

#include <stdint.h>
#include "esp_at_types.h"
#include "main.h"

typedef struct {
    UART_HandleTypeDef *huart;       // &huart2
    DMA_HandleTypeDef  *hdma_rx;     // &hdma_usart2_rx
    DMA_HandleTypeDef  *hdma_tx;     // &hdma_usart2_tx
    GPIO_TypeDef       *en_port;
    uint16_t            en_pin;
    GPIO_TypeDef       *rst_port;
    uint16_t            rst_pin;
    uint32_t            baud;        // 默认 115200
} esp_at_port_config_t;

/**
 * @brief 初始化 ESP 控制脚（EN=LOW, RST=HIGH）
 *
 * @param cfg  端口配置
 */
void          esp_at_esp_port_gpio_init(const esp_at_port_config_t *cfg);

/**
 * @brief 硬件复位 ESP32
 *
 * @param cfg  端口配置
 */
void          esp_at_esp_port_reset    (const esp_at_port_config_t *cfg);

/**
 * @brief ESP32 下电（EN=LOW）
 *
 * @param cfg  端口配置
 */
void          esp_at_esp_port_power_off(const esp_at_port_config_t *cfg);

/**
 * @brief ESP32 硬复位（拉低 EN 100ms → 拉高 → 等 boot）
 *
 * 用于 ESP-AT 软死锁时从 STM32 端恢复。
 * cfg->en_port 为 NULL 时跳过（不报错），调用方需自己处理。
 *
 * @param cfg           端口配置
 * @param boot_wait_ms   复位后等待 boot 时间（0 → 默认 8000ms）
 * @return ESP_AT_OK / ERR_INVALID_ARG
 */
esp_at_err_t  esp_at_esp_port_hard_reset(const esp_at_port_config_t *cfg, uint32_t boot_wait_ms);

/**
 * @brief boot 等 ready URC（当前简化为返回 TIMEOUT，由 esp_at_init 走 AT 探测路径）
 *
 * @param cfg         端口配置
 * @param timeout_ms  超时（未使用）
 * @return ERR_TIMEOUT
 */
esp_at_err_t  esp_at_esp_port_boot     (const esp_at_port_config_t *cfg, uint32_t timeout_ms);

/**
 * @brief 启动 UART DMA + IDLE 中断
 *
 * @param cfg  端口配置
 * @return ESP_AT_OK / ERR_*
 */
esp_at_err_t esp_at_port_uart_start(const esp_at_port_config_t *cfg);

/**
 * @brief 停止 UART 接收/发送并清除端口句柄
 *
 * 必须在释放客户端 ringbuffer 前调用，避免 DMA/中断回调访问失效对象。
 */
void          esp_at_port_uart_stop(void);

/**
 * @brief USART2 中断入口转发（由 stm32f4xx_it.c 调用）
 *
 * @param huart  HAL UART 句柄
 */
void esp_at_port_uart_irq_handler(UART_HandleTypeDef *huart);

/**
 * @brief HAL RxEvent 回调注入（IDLE / 半 / 全填充）
 *
 * @param huart  HAL UART 句柄
 * @param size   已接收字节数
 */
void esp_at_port_uart_rx_event(UART_HandleTypeDef *huart, uint16_t size);

/**
 * @brief HAL TxCplt 回调注入
 *
 * @param huart  HAL UART 句柄
 */
void esp_at_port_uart_tx_cplt (UART_HandleTypeDef *huart);

/**
 * @brief 调试：dump rx_rb 全部字节到 RTT
 */
void esp_at_port_uart_rx_dump(void);

// HAL 同步发送：scheduler 损坏环境下的同步发送兜底（HAL_Delay 不依赖 FreeRTOS tick）
typedef enum {
    ESP_AT_PORT_RC_OK = 0,
    ESP_AT_PORT_RC_ERROR,
    ESP_AT_PORT_RC_TIMEOUT,
    ESP_AT_PORT_RC_INVALID,
} esp_at_port_rc_t;

/**
 * @brief HAL 同步发送 + 同步等待响应
 *
 * @param cmd_line      AT 命令
 * @param wait_ms       等待时长
 * @param out_buf       响应填充
 * @param out_buf_sz    响应缓冲大小
 * @return esp_at_port_rc_t
 */
esp_at_port_rc_t esp_at_port_uart_send_and_wait(const char *cmd_line,
                                                 uint32_t wait_ms,
                                                 char *out_buf,
                                                 uint16_t out_buf_sz);

#endif /* ESP_AT_PORT_STM32_H */
