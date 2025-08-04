#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "hw/hw.h"

uint64_t get_uart_base();
void enable_uart();
void uart_puts(const char *s);
void uart_putc(const char c);
void uart_puthex(uint64_t value);

void uart_raw_putc(const char c);
void uart_raw_puts(const char *s);

#ifdef __cplusplus
}
#endif