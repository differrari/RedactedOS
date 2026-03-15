#pragma once

#include "types.h"
#include "hw/hw.h"

void irq_init();
void irq_el1_handler();
#ifdef __cplusplus
extern "C" {
#endif
typedef uint64_t irq_flags_t;

void disable_interrupt();
void enable_interrupt();
irq_flags_t irq_save_disable();
void irq_restore(irq_flags_t flags);
#ifdef __cplusplus
}
#endif