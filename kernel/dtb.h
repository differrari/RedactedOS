#pragma once

#include "types.h"

typedef struct {
    const char *compatible;
    uint64_t reg_base;
    uint64_t reg_size;
    uint32_t irq;
    int found;
} dtb_match_t;

int get_memory_region(uint64_t *out_base, uint64_t *out_size);