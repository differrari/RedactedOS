#pragma once

#include "types.h"

typedef struct {
    char* buffer;
    size_t buffer_size;
    size_t limit;
    bool can_grow;
    bool circular;
    uintptr_t cursor;
} buffer;

buffer buffer_create(size_t size, bool can_grow, bool circular);
void buffer_write(buffer *buf, char* fmt, ...);
void buffer_write_space(buffer *buf);