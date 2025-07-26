#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CQueue {
    void*    buffer;
    uint64_t capacity;      // current queue size
    uint64_t max_capacity;  // 0 = infinite
    uint64_t elem_size;
    uint64_t head;
    uint64_t tail;
    uint64_t length;
} CQueue;

extern uintptr_t malloc(uint64_t);
extern void free(void*, uint64_t);

void     cqueue_init(CQueue* q, uint64_t max_capacity, uint64_t elem_size);
int32_t  cqueue_enqueue(CQueue* q, const void* item);
int32_t  cqueue_dequeue(CQueue* q, void* out);
int32_t  cqueue_is_empty(const CQueue* q);
uint64_t cqueue_size(const CQueue* q);
void     cqueue_clear(CQueue* q);
void     cqueue_destroy(CQueue* q);

#ifdef __cplusplus
}
#endif
