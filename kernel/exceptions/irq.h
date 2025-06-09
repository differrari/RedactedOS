#pragma once

#include "types.h"

#define GICD_BASE 0x08000000
#define GICC_BASE 0x08010000

void irq_init();
void irq_el1_handler();
void disable_interrupt();
void enable_interrupt();
