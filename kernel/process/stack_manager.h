#pragma once

#include "types.h"
#include "process.h"

#define stack_max            0x100000
#define kstack_max            0x10000
#define stack_max_addr 0x7ffffffff000ULL
#define stack_distance   (stack_max + PAGE_SIZE)
#define stack_min_addr 0x100000000000ULL

stack_t new_stack(process_t *proc);
void unmap_stack(process_t *proc, stack_t stack);