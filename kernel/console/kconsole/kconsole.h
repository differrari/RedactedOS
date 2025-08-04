#pragma once

#include "process/process.h"
#include "kernel_processes/kprocess_loader.h"

#ifdef __cplusplus
extern "C" {
#endif

void kconsole_putc(char c);
void kconsole_puts(const char *s);
void kconsole_clear();
process_t* start_terminal();

#ifdef __cplusplus
}
#endif
