#pragma once

#include "types.h"

#define printf(fmt, ...) \
    ({ \
        uint64_t _args[] = { __VA_ARGS__ }; \
        printf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

void printf_args(const char *fmt, const uint64_t *args, uint32_t arg_count);
void puts(const char *s);
void putc(const char c);
void puthex(uint64_t value);
void disable_visual();
void enable_visual();