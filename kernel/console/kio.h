#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

void kprintf(const char *fmt, ...);
void kprint(const char *fmt);

void kputf(const char *fmt, ...);
void puts(const char *s);
void putc(const char c);

void disable_visual();
void enable_visual();

#ifdef __cplusplus
}
#endif