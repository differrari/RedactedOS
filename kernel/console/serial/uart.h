#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "hw/hw.h"

extern u32 UART_IRQ;

void enable_uart();
void prepare_uart_hw();
void uart_puts(const char *s);
void uart_putc(const char c);
void uart_puthex(uint64_t value);

void uart_raw_putc(const char c);
void uart_raw_puts(const char *s);

u8 uart_get_byte();
void uart_read_in();

extern uint32_t uart_ibrd;
extern uint32_t uart_fbrd;
extern uint32_t uart_baud;

#ifdef __cplusplus
}
#endif