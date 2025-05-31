#include "console/serial/uart.h"
#include "memory/memory_access.h"
#include "interrupts/irq.h"

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

void uart_raw_putc(const char c) {
    while (read32(UART0_FR) & (1 << 5));
    write32(UART0_DR, c);
}

void uart_putc(const char c){
    uart_raw_putc(c);
}

void uart_puts(const char *s) {
    uart_raw_puts(s);
}

void uart_raw_puts(const char *s) {
    while (*s != '\0') {
        uart_raw_putc(*s);
        s++;
    }
}

void uart_puthex(uint64_t value) {
    bool started = false;
    uart_raw_putc('0');
    uart_raw_putc('x');
    for (int i = 60; i >= 0; i -= 4) {
        uint8_t nibble = (value >> i) & 0xF;
        char curr_char = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
        if (started || curr_char != '0' || i == 0) {
            started = true;
            uart_raw_putc(curr_char);
        }
    }
}