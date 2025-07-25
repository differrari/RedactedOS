#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CRingBuffer {
    void* buffer;
    uint64_t capacity;
    uint64_t element_size;
    uint64_t head;
    uint64_t tail;
    int32_t full;
} CRingBuffer;

void     cring_init(CRingBuffer* rb, void* storage, uint64_t capacity, uint64_t elem_size);
int32_t  cring_push(CRingBuffer* rb, const void* item);
int32_t  cring_pop(CRingBuffer* rb, void* out);
int32_t  cring_is_empty(const CRingBuffer* rb);
int32_t  cring_is_full(const CRingBuffer* rb);
void     cring_clear(CRingBuffer* rb);
uint64_t cring_capacity(const CRingBuffer* rb);

#ifdef __cplusplus
}
#endif
