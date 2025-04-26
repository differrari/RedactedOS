#include "console/serial/uart.h"
#include "ram_e.h"
#include "gic.h"

#define UART0_DR   (UART0_BASE + 0x00)
#define UART0_FR   (UART0_BASE + 0x18)
#define UART0_IBRD (UART0_BASE + 0x24)
#define UART0_FBRD (UART0_BASE + 0x28)
#define UART0_LCRH (UART0_BASE + 0x2C)
#define UART0_CR   (UART0_BASE + 0x30)

uint64_t get_uart_base(){
    return UART0_BASE;
}

void enable_uart() {
    write32(UART0_CR, 0x0);

    write32(UART0_IBRD, 1);
    write32(UART0_FBRD, 40);

    write32(UART0_LCRH, (1 << 4) | (1 << 5) | (1 << 6));

    write32(UART0_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

void uart_putc(const char c) {
    while (read32(UART0_FR) & (1 << 5));
    write32(UART0_DR, c);
}

void uart_puts(const char *s) {
    while (*s != '\0') {
        uart_putc(*s);
        s++;
    }
}

void uart_puthex(uint64_t value) {
    const char hex_chars[] = "0123456789ABCDEF";
    bool started = false;
    uart_putc('0');
    uart_putc('x');
    for (int i = 60; i >= 0; i -= 4) {
        char curr_char = hex_chars[(value >> i) & 0xF];
        if (started || curr_char != '0' || i == 0) {
            started = true;
            uart_putc(curr_char);
        }
    }
}