#pragma once

#include "types.h"
#include "hw/hw.h"

void irq_init();
void irq_el1_handler();
void disable_interrupt();
void enable_interrupt();
