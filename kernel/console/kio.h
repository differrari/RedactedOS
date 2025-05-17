#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

#define kprintf(fmt, ...) \
    ({ \
        uint64_t _args[] = { __VA_ARGS__ }; \
        kprintf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

#define kprintf_raw(fmt, ...) \
    ({ \
        uint64_t _args[] = { __VA_ARGS__ }; \
        kprintf_args_raw((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

#define kputf_raw(fmt, ...) \
    ({ \
        uint64_t _args[] = { __VA_ARGS__ }; \
        kputf_args_raw((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

void kprintf_args(const char *fmt, const uint64_t *args, uint32_t arg_count);
void kprintf_args_raw(const char *fmt, const uint64_t *args, uint32_t arg_count);
void kputf_args_raw(const char *fmt, const uint64_t *args, uint32_t arg_count);
void puts(const char *s);
void putc(const char c);
void puthex(uint64_t value);
void disable_visual();
void enable_visual();

#ifdef __cplusplus
}
#endif