#ifndef UART_H
#define UART_H

#include "types.h"

void enable_uart();
void uart_puts(const char *s);
void uart_putc(const char c);
void uart_puthex(uint64_t value);

#endif