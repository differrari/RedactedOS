#include "types.h"
#include "ring_buffer.hpp"
#include "std/memfunctions.h"

void cring_init(struct CRingBuffer* rb, void* storage, uint64_t capacity, uint64_t elem_size){
    rb->buffer = storage;
    rb->capacity = capacity;
    rb->element_size = elem_size;
    rb->head = 0;
    rb->tail = 0;
    rb->full = 0;
}
uint64_t cring_capacity(const struct CRingBuffer* rb){
    return rb->capacity;
}
int32_t cring_push(struct CRingBuffer* rb, const void* item){
    if (rb->full) return 0;

    uint8_t* base = (uint8_t*)rb->buffer;
    void* dest = base + (rb->head * rb->element_size);

    for(uint32_t i = 0; i < rb->element_size; ++i){
        ((uint8_t*)dest)[i] =((const uint8_t*)item)[i];
    }

    rb->head = (rb->head + 1) % rb->capacity;
    rb->full = (rb->head == rb->tail);
    return 1;
}

int32_t cring_pop(struct CRingBuffer* rb, void* out){
    if(!rb->full && (rb->head == rb->tail)) return 0;

    uint8_t* base = (uint8_t*)rb->buffer;
    void* src = base + (rb->tail * rb->element_size);

    for(uint64_t i = 0; i < rb->element_size; ++i){
        ((uint8_t*)out)[i] = ((uint8_t*)src)[i];
    }

    rb->tail =(rb->tail + 1) % rb->capacity;
    rb->full =0;
    return 1;
}

int32_t cring_is_empty(const struct CRingBuffer* rb){
    return (!rb->full && (rb->head == rb->tail));
}

int32_t cring_is_full(const struct CRingBuffer* rb){
    return rb->full;
}

void cring_clear(struct CRingBuffer* rb){
    rb->head = 0;
    rb->tail = 0;
    rb->full = 0;
}
