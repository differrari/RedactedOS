#pragma once

#include "types.h"
#include "hw/hw.h"

#define MBOX_READ       (*(volatile uint32_t*)(MAILBOX_BASE + 0x00))
#define MBOX_STATUS     (*(volatile uint32_t*)(MAILBOX_BASE + 0x18))
#define MBOX_WRITE      (*(volatile uint32_t*)(MAILBOX_BASE + 0x20))

#define MBOX_FULL       0x80000000
#define MBOX_EMPTY      0x40000000

#define MBOX_VC_BASE 0x40000
#define MBOX_VC_FRAMEBUFFER_TAG (MBOX_VC_BASE + 0x1)
#define MBOX_VC_PHYS_SIZE_TAG (MBOX_VC_BASE + 0x3)
#define MBOX_VC_VIRT_SIZE_TAG (MBOX_VC_BASE + 0x4)
#define MBOX_VC_DEPTH_TAG (MBOX_VC_BASE + 0x5)
#define MBOX_VC_FORMAT_TAG (MBOX_VC_BASE + 0x6)
#define MBOX_VC_ALPHA_TAG (MBOX_VC_BASE + 0x7)
#define MBOX_VC_PITCH_TAG (MBOX_VC_BASE + 0x8)
#define MBOX_VC_OFFSET_TAG (MBOX_VC_BASE + 0x9)

#define MBOX_CLKRATE_TAG 0x30002

#define MBOX_SET_VALUE 0x8000

#ifdef __cplusplus
extern "C" {
#endif
int mailbox_call(volatile uint32_t* mbox, uint8_t channel);
#ifdef __cplusplus
}
#endif