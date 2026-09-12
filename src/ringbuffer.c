/**
 * @file    ringbuffer.c
 * @brief   单写单读无锁环形缓冲：size 必须 2 的幂；head/tail uint16_t 回绕
 */

#include "ringbuffer.h"
#include <string.h>

// 初始化环形缓冲。
void ringbuffer_init(ringbuffer_t *rb, uint8_t *storage, uint16_t size)
{
    rb->buf  = storage;
    rb->size = size;
    rb->mask = (uint16_t)(size - 1u);
    rb->head = 0;
    rb->tail = 0;
}

// 已就绪字节数（head - tail）
uint16_t ringbuffer_available(const ringbuffer_t *rb)
{
    return (uint16_t)(rb->head - rb->tail);
}

// 剩余空间（size - available）
uint16_t ringbuffer_free_space(const ringbuffer_t *rb)
{
    return (uint16_t)(rb->size - ringbuffer_available(rb));
}

// 是否空（head == tail）
bool ringbuffer_is_empty(const ringbuffer_t *rb)
{
    return rb->head == rb->tail;
}

// 是否满（available == size）
bool ringbuffer_is_full(const ringbuffer_t *rb)
{
    return ringbuffer_available(rb) == rb->size;
}

// 写入（ISR 安全）：不足时截断，返回实际写入字节数
uint16_t ringbuffer_write(ringbuffer_t *rb, const uint8_t *src, uint16_t len)
{
    uint16_t free = ringbuffer_free_space(rb);
    if (len > free) len = free;
    uint16_t h = rb->head;
    for (uint16_t i = 0; i < len; i++) {
        rb->buf[(h + i) & rb->mask] = src[i];
    }
    rb->head = (uint16_t)(h + len);
    return len;
}

// 读出（推进 tail）：不足时返回实际读取字节数
uint16_t ringbuffer_read(ringbuffer_t *rb, uint8_t *dst, uint16_t len)
{
    uint16_t avail = ringbuffer_available(rb);
    if (len > avail) len = avail;
    uint16_t t = rb->tail;
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = rb->buf[(t + i) & rb->mask];
    }
    rb->tail = (uint16_t)(t + len);
    return len;
}

// 非破坏读（不推进 tail）
uint16_t ringbuffer_peek(ringbuffer_t *rb, uint8_t *dst, uint16_t len)
{
    uint16_t avail = ringbuffer_available(rb);
    if (len > avail) len = avail;
    uint16_t t = rb->tail;
    for (uint16_t i = 0; i < len; i++) {
        dst[i] = rb->buf[(t + i) & rb->mask];
    }
    return len;
}

// 丢弃 len 字节（仅推进 tail）
void ringbuffer_discard(ringbuffer_t *rb, uint16_t len)
{
    uint16_t avail = ringbuffer_available(rb);
    if (len > avail) len = avail;
    rb->tail = (uint16_t)(rb->tail + len);
}

// 找首个 ch：返回偏移，-1 表示未找到
int ringbuffer_find_char(const ringbuffer_t *rb, char ch)
{
    uint16_t avail = ringbuffer_available(rb);
    uint16_t t = rb->tail;
    for (uint16_t i = 0; i < avail; i++) {
        if ((char)rb->buf[(t + i) & rb->mask] == ch) {
            return (int)i;
        }
    }
    return -1;
}
