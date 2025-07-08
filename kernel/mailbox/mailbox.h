#pragma once

#include "types.h"
#include "hw/hw.h"

#define MAILBOX_BASE    (MMIO_BASE + 0xB880)

#define MBOX_READ       (*(volatile uint32_t*)(MAILBOX_BASE + 0x00))
#define MBOX_STATUS     (*(volatile uint32_t*)(MAILBOX_BASE + 0x18))
#define MBOX_WRITE      (*(volatile uint32_t*)(MAILBOX_BASE + 0x20))

#define MBOX_FULL       0x80000000
#define MBOX_EMPTY      0x40000000

#ifdef __cplusplus
extern "C" {
#endif
int mailbox_call(volatile uint32_t* mbox, uint8_t channel);
#ifdef __cplusplus
}
#endif