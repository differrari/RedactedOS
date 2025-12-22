#include "mailbox/mailbox.h"
#include "memory/mmu.h"
#include "console/serial/uart.h"
#include "async.h"

void mailbox_init(){
    register_device_memory(MAILBOX_BASE, MAILBOX_BASE);
}

int mailbox_call(volatile uint32_t* mbox, uint8_t channel) {
    uint32_t addr = ((uint32_t)(uintptr_t)mbox) & ~0xF;
    if (!wait(MBOX_STATUS, MBOX_FULL, false, 200)){
        uart_puts("[MAILBOX] could not find free mailbox slot\n");
        return false;
    }
    MBOX_WRITE = addr | (channel & 0xF);

    while (1) {
        if (!wait(MBOX_STATUS, MBOX_EMPTY, false, 200)){
            uart_puts("[MAILBOX] No response received\n");
            return false;
        }
        uint32_t resp = MBOX_READ;
        if ((resp & 0xF) == channel && (resp & ~0xF) == addr)
            return mbox[1] == 0x80000000;
        else {
            uart_puts("[MAILBOX] Wrong respnonse ");
            uart_puthex(resp);
            return false;
        }
    }
}