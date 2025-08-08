#pragma once

#include "types.h"
#include "dev/driver_base.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __GNUC__
#define PRINTF_FORMAT __attribute__((format(printf, 1, 2)))
#else
#define PRINTF_FORMAT
#endif

PRINTF_FORMAT
void kprintf(const char *fmt, ...);
void kprint(const char *fmt);

PRINTF_FORMAT
void kputf(const char *fmt, ...);
void puts(const char *s);
void putc(const char c);

void disable_visual();
void enable_visual();

extern driver_module console_module;

#ifdef __cplusplus
}
#endif
