#pragma once

#include "types.h"

#define GICD_BASE 0x08000000
#define GICC_BASE 0x08010000

void gic_init();
void timer_init(uint64_t msecs);
void irq_el1_handler();
void enable_interrupt();
