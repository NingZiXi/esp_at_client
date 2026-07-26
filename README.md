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

用户配置覆盖：在工程根 `main/esp_at_config_user.h` 里 `#define` 重新定义宏即可，CMake 会自动 `-include` 注入。

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

## 已知坑

1. **ESP32-C3 上电自动重连 → 别主动 CWJAP**。已 GOT_IP 时主动 CWJAP 会踢掉刚拿的 IP，触发 `WIFI DISCONNECT`。用 `esp_at_wifi_query_state()` 探一下，已连就跳过；`esp_at_wifi_get_state()` 是 cached，boot 后立即调不准。
2. **`esp_at_port_uart_send_and_wait` 只在 boot 期用**。任务起来后跟 `rx_task` 抢同一个 ringbuffer，响应会被偷走。任务起来后用 `esp_at_cmd_send_sync` / 各服务封装（`esp_at_wifi_*` 等）。
3. **HTTP `resp.status` 永远是 0**。ESP-AT `+HTTPCLIENT:<size>,<body>` 内部解析掉了 status line，只透传 body，判定成功看 body 内容。