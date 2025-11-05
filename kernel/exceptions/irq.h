#pragma once

#include "types.h"
#include "hw/hw.h"

void irq_init();
void irq_el1_handler();
#ifdef __cplusplus
extern "C" {
#endif
void disable_interrupt();
void enable_interrupt();
#ifdef __cplusplus
}
#endif