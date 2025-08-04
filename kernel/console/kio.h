#pragma once

#include "types.h"
#include "dev/driver_base.h"
#include "kernel_processes/kprocess_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

void kprintf(const char *fmt, ...);
void kprint(const char *fmt);

void kputf(const char *fmt, ...);
void puts(const char *s);
void putc(const char c);

void disable_visual();
void enable_visual();

process_t* start_terminal();

extern driver_module console_module;

#ifdef __cplusplus
}
#endif