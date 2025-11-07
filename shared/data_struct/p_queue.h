#pragma once

#include "types.h"

//TODO: completely unoptimized. Heap?

//lower number = higher priority
typedef struct p_queue_item {
    void* ptr;
    uint64_t val;
} p_queue_item;

typedef struct p_queue_t {
    p_queue_item *array;
    int size, max_size;
    uint64_t max_priority;
    int max_priority_index;
} p_queue_t;

bool p_queue_insert(p_queue_t *root, void* ptr, uint64_t value);
int p_queue_peek(p_queue_t*root);
void* p_queue_pop(p_queue_t *root);
void p_queue_traverse(p_queue_t *root);
p_queue_t* p_queue_create(int max);
void p_queue_free(p_queue_t *root);