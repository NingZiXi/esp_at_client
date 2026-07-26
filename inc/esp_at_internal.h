/**
 * @file    esp_at_internal.h
 * @brief   AT Client 行解析 + 状态机（内部接口）
 */

#ifndef ESP_AT_INTERNAL_H
#define ESP_AT_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_at_types.h"
#include "esp_at_config_default.h"
#include "ringbuffer.h"
#include "esp_at_link.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "event_groups.h"
#include "task.h"

// 客户端状态
typedef enum {
    ESP_AT_STATE_IDLE          = 0,
    ESP_AT_STATE_SENDING       = 1,
    ESP_AT_STATE_WAITING       = 2,
    ESP_AT_STATE_DATA_PROMPT   = 3,
    ESP_AT_STATE_RAW_TX        = 4,
    ESP_AT_STATE_RAW_RX        = 5,
    ESP_AT_STATE_ERROR         = 6,
} esp_at_state_t;

// 内部挂起命令上下文
typedef struct {
    at_cmd_response_t *resp;
    uint32_t           deadline_ms;
    uint8_t            retry_left;
    bool               is_data_prompt;           // 当前命令是否期待 '>'
} at_pending_cmd_t;

// 客户端全局对象（单例，在 esp_at_internal.c 定义）
typedef struct esp_at_client {
    esp_at_link_t    *link;
    ringbuffer_t      rx_rb;
    uint8_t          *rx_storage;
    uint8_t          *resp_line_buf;
    uint16_t          resp_line_buf_sz;

    esp_at_state_t    state;
    volatile uint32_t state_enter_ms;

    at_pending_cmd_t  pending;                   // pending.resp==NULL 表示 IDLE
    bool              echo_pending;              // 等待 echo 回串

    const uint8_t    *raw_tx_buf;                // DATA_PROMPT/RAW_TX
    uint16_t          raw_tx_len;
    uint16_t          raw_tx_off;
    uint32_t          raw_tx_deadline_ms;

    uint8_t          *raw_rx_buf;                // RAW_RX：malloc'd, MQTTSUBRECV/HTTPCLIENT body
    uint16_t          raw_rx_expected;
    uint16_t          raw_rx_off;

    char              parse_line[ESP_AT_LINE_MAX]; // URC 行缓冲
    uint16_t          parse_line_len;

    TaskHandle_t      rx_task_h;
    TaskHandle_t      tx_task_h;
    TaskHandle_t      evt_task_h;
    QueueHandle_t     urc_queue;
    SemaphoreHandle_t cmd_done_sem;
    EventGroupHandle_t boot_eg;
#define BOOT_READY_BIT  (1u << 0)

// AT 通信细节统一 tag：所有 << TX / >> RX 原始数据用此
#define ESP_AT_PROTO_TAG "at_comms"

    esp_at_event_cb_t cbs[ESP_AT_EVENT_MAX];     // cbs[evt]，evt==ESP_AT_EVENT_MAX 时 NULL
    void             *cb_user[ESP_AT_EVENT_MAX];

    esp_at_event_cb_t cbs_any;                   // ESP_AT_EVENT_ANY 通配（-1 不能做 cbs[] 下标）
    void             *cb_user_any;

    esp_at_wifi_state_t wifi_state;              // 服务层查询

    bool               mqtt_connected;
    bool               inited;
} esp_at_client_t;

extern esp_at_client_t g_esp_at_client;

/**
 * @brief 单例获取
 *
 * @return esp_at_client_t*  全局对象指针
 */
esp_at_client_t *esp_at_client_get(void);

/**
 * @brief 客户端初始化（分配 ringbuffer / 同步原语）
 *
 * @param link  链路实例
 * @return ESP_AT_OK / ERR_NO_MEM / ERR_INVALID_ARG
 */
esp_at_err_t esp_at_client_init   (esp_at_link_t *link);

/**
 * @brief 客户端反初始化（释放资源）
 *
 * @return ESP_AT_OK
 */
esp_at_err_t esp_at_client_deinit (void);

/**
 * @brief 启动 rx / tx / evt 任务
 */
void         esp_at_client_start_tasks(void);

/**
 * @brief RX 任务入口（行解析）
 *
 * @param arg  FreeRTOS task arg（未用）
 */
void esp_at_client_rx_task(void *arg);

/**
 * @brief 同步发送：拼装 cmd_line + CRLF，写 link，返回码由 resp 填充
 *
 * @param cmd_line    AT 命令
 * @param resp        响应填充
 * @param timeout_ms  超时（ms）
 * @return ESP_AT_OK / ERR_TIMEOUT / ERR_BUSY 等
 */
esp_at_err_t esp_at_client_send_sync(const char *cmd_line,
                                     at_cmd_response_t *resp,
                                     uint32_t timeout_ms);

/**
 * @brief DMA RX 完成回调中调用
 */
void esp_at_client_notify_rx(void);

/**
 * @brief DMA TX 完成回调中调用
 */
void esp_at_client_notify_tx(void);

/**
 * @brief 行解析匹配出来的 URC 转事件投递
 *
 * @param evt      事件类型
 * @param payload  事件 payload（可 NULL）
 * @return ESP_AT_OK / ERR_INVALID_ARG / ERR_NOT_READY
 */
esp_at_err_t esp_at_client_post_event(esp_at_event_t evt, const esp_at_event_payload_t *payload);

/**
 * @brief boot 同步原语：设置 ready 位
 */
void esp_at_client_set_ready(void);

/**
 * @brief boot 同步原语：查询 ready
 *
 * @return true 已 ready
 */
bool esp_at_client_is_ready(void);

/**
 * @brief boot 同步原语：阻塞等 ready
 *
 * @param timeout_ms  超时
 * @return 实际等待时间（ms）
 */
uint32_t esp_at_client_wait_ready(uint32_t timeout_ms);

/**
 * @brief 应用层在两次 AT 命令之间的空闲期周期调用，让 URC 及时被 dispatch
 *
 * 详细：scheduler 损坏环境下 rx_task 不被调度，pump 由调用方接管
 */
void esp_at_client_pump_rx(void);

/**
 * @brief 行匹配工具：检查 line 是否以 prefix 开头
 *
 * @param line    待匹配行
 * @param prefix  前缀字符串
 * @return true 匹配
 */
bool esp_at_match_prefix(const char *line, const char *prefix);

/**
 * @brief 行匹配工具：在 line 中找 key 并解析其后整数
 *
 * @param line  待解析行
 * @param key   关键字（含或不含 =/:）
 * @param out   输出整数
 * @return true 找到且解析成功
 */
bool esp_at_extract_int (const char *line, const char *key, int *out);

/**
 * @brief 行匹配工具：在 line 中找 "key":"..."
 *
 * @param line    待解析行
 * @param key     关键字
 * @param out     输出缓冲
 * @param out_sz  输出缓冲大小
 * @return true 找到
 */
bool esp_at_extract_str (const char *line, const char *key, char *out, uint16_t out_sz);

#endif /* ESP_AT_INTERNAL_H */