# esp_at_client

STM32 跑 ESP-AT 固件的客户端库，把 ESP-AT UART 行协议封装成 WiFi / HTTP / MQTT 的同步 + 事件回调 API。

## 目录

```
esp_at_client/
├── CMakeLists.txt
├── inc/        对外头文件（API、类型、配置）
├── src/        实现
└── example/    参考示例（[README](example/README.md)）
```

详细 API 看 `inc/` 下对应头文件；事件 / payload / 配置宏看 [esp_at_types.h](inc/esp_at_types.h) 和 [esp_at_config_default.h](inc/esp_at_config_default.h)。

## 接入

工程根 CMakeLists：

```cmake
include(FetchContent)
FetchContent_Declare(
    stm_log
    GIT_REPOSITORY https://gitee.com/nzxhg/stm_log.git
    #GIT_REPOSITORY https://github.com/NingZiXi/stm_log.git
    GIT_TAG        v2.3.1
    SOURCE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/Lib/stm_log
)
FetchContent_MakeAvailable(stm_log)

add_subdirectory(Lib/esp_at_client)
target_link_libraries(your_app PRIVATE esp_at_client)
```

依赖：`stm32cubemx` target（CubeMX 生成的 HAL/FreeRTOS/CMSIS）、USART2 + 两路 DMA（CubeMX 配置好）、[stm_log](https://github.com/NingZiXi/stm_log)（日志后端，必需）。

### 用户配置覆盖

`main/esp_at_config_user.h` **默认不存在**——库自动 `-include` 这个文件,有就 inject、没就 fallback 到 default。

要覆盖某个宏时：

```c
/**
 * @file    esp_at_config_user.h
 * @brief   项目级用户配置
 */

#ifndef ESP_AT_CONFIG_USER_H
#define ESP_AT_CONFIG_USER_H

// 例：把任务栈调大
#undef ESP_AT_TASK_RX_STACK
#define ESP_AT_TASK_RX_STACK    1024

// 例：打开 at_comms tag VERBOSE 级，看 << / >>
#undef ESP_AT_COMMS_VERBOSE_LOG
#define ESP_AT_COMMS_VERBOSE_LOG 1

#endif /* ESP_AT_CONFIG_USER_H */
```

要点：
- 用 `#undef + #define` **显式覆盖**——`default.h` 已经 `#define`，不能用 `#ifndef` 跳过
- 想看有哪些可覆盖宏，看 [esp_at_config_default.h](inc/esp_at_config_default.h)
- 新加的宏用 `#ifndef` 模式（只有 user.h 给默认）

## 最小示例

```c
#include "esp_at_client.h"
#include "esp_at_wifi.h"
#include "esp_at_http.h"

extern UART_HandleTypeDef huart2;
extern DMA_HandleTypeDef  hdma_usart2_rx;
extern DMA_HandleTypeDef  hdma_usart2_tx;

static const esp_at_port_config_t s_port = {
    .huart   = &huart2,
    .hdma_rx = &hdma_usart2_rx,
    .hdma_tx = &hdma_usart2_tx,
    .baud    = 115200,
};

void app_main(void) {
    esp_at_register_event_cb(ESP_AT_EVENT_ANY, on_event, NULL);
    if (esp_at_init(&s_port) != ESP_AT_OK)               return;
    if (esp_at_wifi_init(1) != ESP_AT_OK)                return;

    esp_at_wifi_query_t q = {0};
    if (esp_at_wifi_query_state(&q, 1500) != ESP_AT_OK  // ← 已 GOT_IP 就别 CWJAP
        || q.state != ESP_AT_WIFI_GOT_IP) {
        if (esp_at_wifi_connect("SSID", "PSK", 15000) != ESP_AT_OK) return;
    }

    esp_at_http_resp_t resp = {0};
    esp_at_http_get("http://your-server/get", &resp, 5000);
    // resp.body 由调用方 free
    if (resp.body) vPortFree(resp.body);
}
```

更完整的 demo（MQTT / event callback 等）见 [example/](example/)。

## 日志

5 档对齐 stm_log：`ESP_AT_LOG_LEVEL` = 0=off 1=err 2=warn 3=info 4=debug 5=verbose（默认 3）。

AT 通信细节统一打 `at_comms` tag，前缀 `<<` 是 MCU 发出，`>>` 是 ESP32 回的（一律原始数据）：

```
I demo: GET ... status=200 ...
D at_comms: << AT+HTTPCLIENT=2,0,"http://..."
D at_comms: >> +HTTPCLIENT:32,hello from stm_ota_server GET /
V at_comms: << rc=0, resp=37 bytes
V at_comms: >> echo POST /post ok, received 5 bytes
```

stm_log 的 per-tag 级别**覆盖**全局（见 stm_log.c 的 `resolve_level`）—— 调试时只开 `at_comms` 不影响其他 tag：

```c
esp_at_log_set_tag_level("at_comms", STM_LOG_LVL_DEBUG);   // 开 << / >>
esp_at_log_set_tag_level("at_comms", STM_LOG_LVL_NONE);    // 全关
esp_at_log_unset_tag_level("at_comms");                    // 恢复全局默认
esp_at_log_set_level(STM_LOG_LVL_DEBUG);                   // 一次性升全局
```

调试完无需动 config：per-tag 设完即生效，重启后由 `esp_at_log_set_tag_level` 调用恢复。
