#include "mailbox.h"

int mailbox_call(volatile uint32_t* mbox, uint8_t channel) {
    uint32_t addr = ((uint32_t)(uintptr_t)mbox) & ~0xF;

    while (MBOX_STATUS & MBOX_FULL);
    MBOX_WRITE = addr | (channel & 0xF);

    while (1) {
        while (MBOX_STATUS & MBOX_EMPTY);
        uint32_t resp = MBOX_READ;
        if ((resp & 0xF) == channel && (resp & ~0xF) == addr)
            return mbox[1] == 0x80000000;
        else return false;
    }
}