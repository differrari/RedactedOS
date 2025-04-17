#include "uart.h"
#include "mmio.h"

#define UART0DR 0x09000000

void uart_putc(const char c){
    mmio_write(UART0DR,c);
}

void uart_puts(const char *s) {
    while(*s != '\0') {
        uart_putc(*s);
        s++;
    }
}

void uart_puthex(uint64_t value) {
    const char hex_chars[] = "0123456789ABCDEF";
    bool started = false;
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) {
        char curr_char = hex_chars[(value >> i) & 0xF];
        if (started || curr_char != '0' || i == 0){
            started = true;
            uart_putc(curr_char);
        }
    }
}