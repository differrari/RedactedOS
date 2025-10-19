#pragma once

#include "types.h"

typedef struct {
    uintptr_t address;
    uint32_t line;
    uint32_t column;
    const char *file;
} debug_line_info;

debug_line_info dwarf_decode_lines(uintptr_t ptr, size_t size, uintptr_t debug_line_str_base, size_t str_size, uintptr_t address);