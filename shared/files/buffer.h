#pragma once

#include "types.h"
#include "std/args.h"

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
void buffer_write_va(buffer *buf, char* fmt, va_list args);
void buffer_write_const(buffer *buf, char *lit);
void buffer_write_lim(buffer *buf, char *lit, size_t size);
void buffer_write_space(buffer *buf);