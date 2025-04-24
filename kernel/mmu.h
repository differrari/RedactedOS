#pragma once

#include "types.h"

void mmu_init();
void register_proc_memory(uint64_t va, uint64_t pa);