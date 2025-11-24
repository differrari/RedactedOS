#pragma once

#include "types.h"
#include "dev/driver_base.h"
#include "kernel_processes/kprocess_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

void kprintf(const char *fmt, ...); //TODO __attribute__((format(printf, 1, 2)));
void kprint(const char *fmt);

void kputf(const char *fmt, ...); //TODO __attribute__((format(printf, 1, 2)));
void puts(const char *s);
void putc(const char c);

void disable_visual();
void enable_visual();

extern system_module console_module;

#ifdef __cplusplus
}
#endif