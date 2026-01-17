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

volatile uint32_t uart_mbox[9] __attribute__((aligned(16))) = {
    36, 0, MBOX_CLKRATE_TAG, 12, 8, 2, 0, 0, 0
};

void prepare_uart_hw() {
    
    if (RPI_BOARD != 5){
        reset_gpio();
        enable_gpio_pin(14);
        enable_gpio_pin(15);
    }
    if (RPI_BOARD >= 4) 
        if (mailbox_call(uart_mbox,8)){
            uint32_t uart_clk = uart_mbox[6];
            uart_ibrd = uart_clk / (16 * uart_baud);
            uint32_t rem = uart_clk % (16 * uart_baud);
            uart_fbrd = (rem * 64 + uart_baud/2) / uart_baud;
        }
}