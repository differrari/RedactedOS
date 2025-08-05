#pragma once

#include "types.h"

void timer_init(uint64_t msecs);
void timer_reset();

void virtual_timer_reset(uint64_t smsecs);
void virtual_timer_enable();
uint64_t virtual_timer_remaining_msec();

uint64_t timer_now();
uint64_t timer_now_msec();

void permanent_disable_timer();