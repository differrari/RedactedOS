#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"

#define UART0_BASE 0x09000000

uint64_t get_uart_base();
void enable_uart();
void uart_puts(const char *s);
void uart_putc(const char c);
void uart_puthex(uint64_t value);

#ifdef __cplusplus
}
#endif