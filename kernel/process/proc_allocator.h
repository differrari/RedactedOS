#pragma once

#include "types.h"

void* alloc_proc_mem(uint64_t size, bool kernel);
void free_proc_mem(void* ptr, uint64_t size);