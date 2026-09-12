/**
 * @file    mqtt_demo.c
 * @brief   ESP-AT 客户端库 MQTT 演示（参考代码）
 *
 * 这段代码演示 esp_at_client 库的完整使用流程：
 *   - 注册事件回调（任意事件）
 *   - 连接 WiFi
 *   - 连接 MQTT broker
 *   - 订阅 topic
 *   - 周期性 publish
 *
 * 用法：
 *   1. 拷贝到你的 main/ 目录下
 *   2. 在 app_main() 里调用 app_esp_at_mqtt_demo_run(&port_cfg)
 *   3. 在 CMakeLists.txt 里 target_sources 加入本文件
 *
 * 注意事项：
 *   - 替换 DEMO_WIFI_SSID / DEMO_WIFI_PSK 为你的实际 WiFi
 *   - 替换 DEMO_MQTT_HOST / DEMO_MQTT_CLIENT_ID 为你的 broker 和 client id
 *   - 需要 FreeRTOS 调度正常工作（osDelay / vTaskDelay 都能用）
 */

#include "esp_at_client.h"
#include "esp_at_wifi.h"
#include "esp_at_mqtt.h"
#include "esp_at_http.h"

#include <stdio.h>
#include <stdint.h>

#include "stm_log.h"

#define DEMO_WIFI_SSID      "your-ssid"        // 你的 WiFi SSID
#define DEMO_WIFI_PSK       "your-password"    // 你的 WiFi 密码
#define DEMO_MQTT_HOST      "broker.emqx.io"
#define DEMO_MQTT_PORT      1883
#define DEMO_MQTT_CLIENT_ID "stm32-client-001"
#define DEMO_MQTT_TOPIC_PUB "stm32/telemetry"
#define DEMO_MQTT_TOPIC_SUB "stm32/control"

static const char *TAG = "demo";

// ESP_AT_EVENT_ANY 通配：分发器会对每个事件额外调用一次 cbs_any。
static void on_any_event(const esp_at_event_payload_t *e, void *user)
{
    (void)user;
    switch (e->type) {
    case ESP_AT_EVENT_WIFI_GOT_IP:
        LOGI(TAG, "WiFi connected, IP obtained");
        break;
    case ESP_AT_EVENT_WIFI_DISCONNECT:
        LOGW(TAG, "WiFi disconnected (err=%lu)", e->err_code);
        break;
    case ESP_AT_EVENT_MQTT_CONNECTED:
        LOGI(TAG, "MQTT connected");
        break;
    case ESP_AT_EVENT_MQTT_DISCONNECTED:
        LOGW(TAG, "MQTT disconnected");
        break;
    case ESP_AT_EVENT_MQTT_MESSAGE:
        LOGI(TAG, "MQTT msg: topic=[%s] data_len=%u data=[%.*s]",
             e->topic ? e->topic : "",
             (unsigned)e->data_len,
             (int)e->data_len,
             e->data ? (const char *)e->data : "");
        break;
    default:
        break;
    }
}

/**
 * @brief demo 入口：注册回调 → 连 WiFi → 连 MQTT → 订阅 → 周期 publish
 */
void app_esp_at_mqtt_demo_run(const esp_at_port_config_t *port_cfg)
{
    esp_at_register_event_cb(ESP_AT_EVENT_ANY, on_any_event, NULL);
    esp_at_init(port_cfg);

    if (esp_at_wifi_init(1) != ESP_AT_OK) {
        LOGE(TAG, "wifi_init failed");
        return;
    }

    // ESP32-C3 上电会自动重连：已 GOT_IP 时跳过 CWJAP
    esp_at_wifi_query_t q = {0};
    if (esp_at_wifi_query_state(&q, 1500) != ESP_AT_OK
        || q.state != ESP_AT_WIFI_GOT_IP) {
        if (esp_at_wifi_connect(DEMO_WIFI_SSID, DEMO_WIFI_PSK, 15000) != ESP_AT_OK) {
            LOGE(TAG, "wifi_connect failed");
            return;
        }
    } else {
        LOGI(TAG, "ESP32 already GOT_IP (ssid=%s), skip CWJAP", q.ssid);
    }

    esp_at_mqtt_user_cfg_t uc = {
        .link_id = 0,
        .scheme  = 1,                              // TCP
        .client_id = DEMO_MQTT_CLIENT_ID,
        .username   = NULL,
        .password   = NULL,
        .disable_clean_session = 0,
        .lwt_topic  = NULL,
        .lwt_msg    = NULL,
        .lwt_qos    = 0,
        .lwt_retain = 0,
        .keepalive_s = 60,
    };
    esp_at_mqtt_conn_cfg_t cc = {
        .link_id = 0,
        .timeout_s = 60,
        .ssl = 0,
        .path = "",
    };
    if (esp_at_mqtt_connect(&uc, &cc, DEMO_MQTT_HOST, DEMO_MQTT_PORT, 15000) != ESP_AT_OK) {
        LOGE(TAG, "mqtt_connect failed");
        return;
    }

    if (esp_at_mqtt_subscribe(0, DEMO_MQTT_TOPIC_SUB, 1, NULL, NULL, 5000) != ESP_AT_OK) {
        LOGW(TAG, "MQTTSUB failed");
    } else {
        LOGI(TAG, "subscribed to %s", DEMO_MQTT_TOPIC_SUB);
    }

    // 周期发布：每 2 秒一帧。
    uint32_t tick = 0;
    for (;;) {
        char payload[64];
        int n = snprintf(payload, sizeof payload, "[tick=%lu up=%lu]",
                         (unsigned long)tick, (unsigned long)HAL_GetTick());
        esp_at_mqtt_publish(0, DEMO_MQTT_TOPIC_PUB,
                            (const uint8_t *)payload, (uint16_t)n,
                            1, 0, 5000);
        tick++;
        osDelay(2000);
    }
}
