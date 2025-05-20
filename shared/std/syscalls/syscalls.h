#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

extern void printf_args(const char *fmt, const uint64_t *args, uint32_t arg_count);

extern uintptr_t malloc(size_t size);

extern void free(void *ptr, size_t size);

#define printf(fmt, ...) \
    ({ \
        uint64_t _args[] = { __VA_ARGS__ }; \
        printf_args((fmt), _args, sizeof(_args) / sizeof(_args[0])); \
    })

#ifdef __cplusplus
}
#endif