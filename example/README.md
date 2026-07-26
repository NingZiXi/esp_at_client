# esp_at_client 库使用示例

本目录是 esp_at_client 库的**参考代码**给需要接入这个库的项目作模板。

## 目录结构

```
example/
├── README.md        ← 本说明
├── mqtt/
│   └── mqtt_demo.c  ← WiFi + MQTT 双向流通
└── http/
    └── http_demo.c  ← WiFi + HTTP GET/POST
```

## 如何接入你的项目

### 1. 拷贝 example 下的 demo 到你的 main/ 目录

```sh
cp Lib/esp_at_client/example/mqtt/mqtt_demo.c main/app_esp_at_demo.c
# 或
cp Lib/esp_at_client/example/http/http_demo.c main/app_esp_at_demo.c
```

### 2. 在你的 app_main() 里调用

```c
extern UART_HandleTypeDef huart2;
extern DMA_HandleTypeDef  hdma_usart2_rx;
extern DMA_HandleTypeDef  hdma_usart2_tx;
extern void app_esp_at_mqtt_demo_run(const esp_at_port_config_t *port_cfg);

static const esp_at_port_config_t s_cfg = {
    .huart   = &huart2,
    .hdma_rx = &hdma_usart2_rx,
    .hdma_tx = &hdma_usart2_tx,
    .en_port = NULL, .en_pin = 0,
    .rst_port = NULL, .rst_pin = 0,
    .baud = 115200,
};

void app_main(void) {
    app_esp_at_mqtt_demo_run(&s_cfg);   // 或 app_esp_at_http_demo_run
    for (;;) { __NOP(); }
}
```

### 3. 把 demo 文件加入你工程的 build（怎么加取决于你的 build 系统：CMake / Makefile / IAR / Keil 都不同）

## 前提条件

- **USART2 + DMA**（RX/TX 各一路）已在 CubeMX 配置好
- **FreeRTOS 调度正常工作**——`osDelay` / `vTaskDelay` 能正确阻塞

## 修改连接信息

每个 demo 顶部都有 `#define` 集中配置 WiFi / broker / URL：

```c
#define DEMO_WIFI_SSID      "your-ssid"        // ← 改这里
#define DEMO_WIFI_PSK       "your-password"    // ← 改这里
#define DEMO_MQTT_HOST      "broker.emqx.io"   // ← 改这里
#define DEMO_MQTT_CLIENT_ID "stm32-client-001" // ← 改这里（要唯一）
```

## 关键 API 速查

### 库生命周期

```c
esp_at_init(port_cfg);                                    // 初始化
esp_at_register_event_cb(ESP_AT_EVENT_ANY, cb, user);      // 通配事件回调
```

### WiFi

```c
esp_at_wifi_init(mode);                                    // 1=STA 2=AP 3=STA+AP
esp_at_wifi_connect(ssid, pwd, timeout_ms);
esp_at_wifi_get_ip(ip, gw, mask);
```

### MQTT

```c
esp_at_mqtt_connect(&uc, &cc, host, port, timeout_ms);
esp_at_mqtt_subscribe(link, topic, qos, NULL, NULL, timeout_ms);
esp_at_mqtt_publish(link, topic, data, len, qos, retain, timeout_ms);
```

### HTTP

```c
esp_at_http_get(url, &resp, timeout_ms);
esp_at_http_post(url, "application/json", body, len, &resp, timeout_ms);
esp_at_http_request(method, url, ct, body, len, &resp, timeout_ms);
```

### 事件回调写法

```c
static void on_any_event(const esp_at_event_payload_t *e, void *user)
{
    switch (e->type) {
    case ESP_AT_EVENT_WIFI_GOT_IP:        /* WiFi 连上 */
        break;
    case ESP_AT_EVENT_MQTT_CONNECTED:     /* MQTT broker 接受连接 */
        break;
    case ESP_AT_EVENT_MQTT_MESSAGE:       /* 收到订阅的 MQTT 消息 */
        // e->topic / e->topic_len / e->data / e->data_len
        break;
    case ESP_AT_EVENT_HTTP_DONE:          /* HTTP 响应 */
        // e->http_status / e->data / e->data_len
        break;
    }
}
```