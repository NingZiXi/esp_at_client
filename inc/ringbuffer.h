/**
 * @file    ringbuffer.h
 * @brief   单写单读无锁环形缓冲
 */

#ifndef RINGBUFFER_H
#define RINGBUFFER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t        *buf;
    uint16_t        size;
    uint16_t        mask;       // size - 1
    volatile uint16_t head;     // 写指针
    volatile uint16_t tail;     // 读指针
} ringbuffer_t;

/**
 * @brief 初始化环形缓冲
 *
 * @param rb       缓冲对象
 * @param storage  存储区
 * @param size     size 必须为 2 的幂
 */
void     ringbuffer_init(ringbuffer_t *rb, uint8_t *storage, uint16_t size);

/**
 * @brief 写入数据
 *
 * @param rb   缓冲对象
 * @param src  数据源
 * @param len  长度
 * @return 实际写入字节数（ISR 安全）
 */
uint16_t ringbuffer_write(ringbuffer_t *rb, const uint8_t *src, uint16_t len);

/**
 * @brief 读出数据（推进 tail）
 *
 * @param rb   缓冲对象
 * @param dst  目标
 * @param len  长度
 * @return 实际读取字节数
 */
uint16_t ringbuffer_read (ringbuffer_t *rb, uint8_t *dst, uint16_t len);

/**
 * @brief 读出数据但不推进 tail
 *
 * @param rb   缓冲对象
 * @param dst  目标
 * @param len  长度
 * @return 实际读取字节数
 */
uint16_t ringbuffer_peek (ringbuffer_t *rb, uint8_t *dst, uint16_t len);

/**
 * @brief 已就绪字节数
 *
 * @param rb  缓冲对象
 * @return 字节数
 */
uint16_t ringbuffer_available(const ringbuffer_t *rb);

/**
 * @brief 剩余空间
 *
 * @param rb  缓冲对象
 * @return 字节数
 */
uint16_t ringbuffer_free_space(const ringbuffer_t *rb);

/**
 * @brief 是否空
 *
 * @param rb  缓冲对象
 * @return true 空
 */
bool     ringbuffer_is_empty(const ringbuffer_t *rb);

/**
 * @brief 是否满
 *
 * @param rb  缓冲对象
 * @return true 满
 */
bool     ringbuffer_is_full (const ringbuffer_t *rb);

/**
 * @brief 丢弃 len 字节（仅推进 tail）
 *
 * @param rb   缓冲对象
 * @param len  丢弃字节数
 */
void     ringbuffer_discard(ringbuffer_t *rb, uint16_t len);

/**
 * @brief 找首个 ch
 *
 * @param rb  缓冲对象
 * @param ch  目标字符
 * @return 偏移，-1 表示未找到
 */
int      ringbuffer_find_char(const ringbuffer_t *rb, char ch);

#endif /* RINGBUFFER_H */