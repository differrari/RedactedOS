#pragma once

#include "types.h"

typedef struct {
    const char *compatible;
    uint64_t reg_base;
    uint64_t reg_size;
    uint32_t irq;
    int found;
} dtb_match_t;

typedef int (*dtb_node_handler)(const char *propname, const void *prop, uint32_t len, dtb_match_t *out);

bool dtb_addresses(uint64_t *start, uint64_t *size);
bool dtb_scan(const char *search_name, dtb_node_handler handler, dtb_match_t *match);