#pragma once

#include "types.h"

#define GICD_BASE 0x08000000
#define GICC_BASE 0x08010000

void irq_init();
void timer_init(uint64_t msecs);
void irq_el1_handler();
void disable_interrupt();
void enable_interrupt();

void permanent_disable_timer();

void virtual_timer_reset(uint64_t smsecs);
void virtual_timer_enable();
uint64_t virtual_timer_remaining_msec();

uint64_t timer_now();

uint64_t timer_now_msec();