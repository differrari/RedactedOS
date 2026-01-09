#include "console/serial/uart.h"
#include "std/memory_access.h"
#include "gpio.h"
#include "hw/hw.h"
#include "mailbox/mailbox.h"
#include "memory/mmu.h"

#define UART0_DR   (UART0_BASE + 0x00)
#define UART0_FR   (UART0_BASE + 0x18)
#define UART0_IBRD (UART0_BASE + 0x24)
#define UART0_FBRD (UART0_BASE + 0x28)
#define UART0_LCRH (UART0_BASE + 0x2C)
#define UART0_CR   (UART0_BASE + 0x30)

#define UART_FIFO 4
#define UART_WLEN 5

#define UART_EN 0
#define UART_TXE 8
#define UART_RXE 9

#define UART_8B_WLEN 0b11

uint32_t uart_ibrd;
uint32_t uart_fbrd;
uint32_t uart_baud;

void enable_uart() {
    register_device_memory(UART0_BASE, UART0_BASE);

    write32(UART0_CR, 0x0);

    uart_ibrd = 1;
    uart_fbrd = 40;
    uart_baud = 115200;
    
    prepare_uart_hw();

    write32(UART0_IBRD, uart_ibrd);
    write32(UART0_FBRD, uart_fbrd);

    write32(UART0_LCRH, (1 << UART_FIFO) | (UART_8B_WLEN << UART_WLEN));

    write32(UART0_CR, (1 << UART_EN) | (1 << UART_TXE) | (1 << UART_RXE));
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