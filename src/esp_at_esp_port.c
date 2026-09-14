/**
 * @file    esp_at_esp_port.c
 * @brief   ESP32-C3 EN / RST 时序控制（EN=LOW/RST=HIGH → EN=HIGH → RST=LOW ≥1ms → RST=HIGH）
 */

#include "esp_at_port_stm32.h"
#include "esp_at_internal.h"
#include "esp_at_config_default.h"

#include "stm32f4xx_hal.h"
#include "cmsis_os.h"

#include "stm_log.h"

static const esp_at_port_config_t *s_cfg;

// 初始化 ESP 控制脚（EN=LOW，RST=HIGH）。
void esp_at_esp_port_gpio_init(const esp_at_port_config_t *cfg)
{
    s_cfg = cfg;
    if (!cfg) return;
    if (cfg->en_port) {
        HAL_GPIO_WritePin(cfg->en_port,  cfg->en_pin,  GPIO_PIN_RESET);
    }
    if (cfg->rst_port) {
        HAL_GPIO_WritePin(cfg->rst_port, cfg->rst_pin, GPIO_PIN_SET);
    }
}

// 对 ESP32 执行硬件复位。
void esp_at_esp_port_reset(const esp_at_port_config_t *cfg)
{
    if (!cfg) return;
    s_cfg = cfg;

    if (!cfg->rst_port && !cfg->en_port) {                          // EN/RST 均未配置：跳过复位，假定 ESP32 已上电。
        LOGW("esp_at_esp_port", "ESP_EN/RST not configured — skip hardware reset");
        return;
    }

    if (cfg->en_port) {                                             // 经典 ESP32：EN 上下电并复位 RST。
        HAL_GPIO_WritePin(cfg->en_port,  cfg->en_pin,  GPIO_PIN_RESET);
        osDelay(100);
        HAL_GPIO_WritePin(cfg->en_port,  cfg->en_pin,  GPIO_PIN_SET);  // 使能
        osDelay(1);
    }
    if (cfg->rst_port) {
        HAL_GPIO_WritePin(cfg->rst_port, cfg->rst_pin, GPIO_PIN_RESET); // 拉低复位
        osDelay(10);
        HAL_GPIO_WritePin(cfg->rst_port, cfg->rst_pin, GPIO_PIN_SET);   // 释放
        return;
    }

    LOGI("esp_at_esp_port", "ESP_RST not exposed; using ESP_EN as cold-reset");
}

// ESP32 下电（EN=LOW）。
void esp_at_esp_port_power_off(const esp_at_port_config_t *cfg)
{
    if (!cfg) return;
    if (cfg->en_port) {
        HAL_GPIO_WritePin(cfg->en_port, cfg->en_pin, GPIO_PIN_RESET);
    }
}

// 硬复位 ESP32：拉低 EN 100ms → 拉高 → 等 boot 8s
esp_at_err_t esp_at_esp_port_hard_reset(const esp_at_port_config_t *cfg, uint32_t boot_wait_ms)
{
    if (!cfg || !cfg->en_port) {
        LOGW("esp_at_esp_port", "hard_reset skipped: en_port not configured");
        return ESP_AT_OK;   // 没接 EN 引脚 = 不报错，让调用方走其他路径
    }
    LOGI("esp_at_esp_port", "hard_reset: EN=LOW 100ms → EN=HIGH → wait %u ms", boot_wait_ms);
    HAL_GPIO_WritePin(cfg->en_port, cfg->en_pin, GPIO_PIN_RESET);   // EN=LOW 断电
    HAL_Delay(100);                                                  // ≥100ms 断电
    HAL_GPIO_WritePin(cfg->en_port, cfg->en_pin, GPIO_PIN_SET);     // EN=HIGH 上电
    HAL_Delay(boot_wait_ms ? boot_wait_ms : 8000);                   // 等 boot
    return ESP_AT_OK;
}

// 等待 boot ready URC（当前简化为返回超时，由 esp_at_init 走 AT 探测路径）。
esp_at_err_t esp_at_esp_port_boot(const esp_at_port_config_t *cfg, uint32_t timeout_ms)
{
    esp_at_esp_port_gpio_init(cfg);
    esp_at_esp_port_reset(cfg);
    LOGI("esp_at", "ESP reset done; skip ready-wait, going to AT probe");
    (void)timeout_ms;
    return ESP_AT_ERR_TIMEOUT;
}
