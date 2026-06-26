#pragma once

#include "types.h"
#include "process.h"

#define stack_max            0x100000
#define stack_max_addr 0x7ffffffff000
#define stack_distance   (stack_max + PAGE_SIZE)
#define stack_min_addr 0x100000000000ULL

typedef struct {
    uptr top;
    size_t max;
    size_t size;
} stack_t;

stack_t new_stack(process_t *proc);