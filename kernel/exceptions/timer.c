#include "timer.h"

static uint64_t _msecs;

void timer_reset() {
    uint64_t freq;
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = (freq * _msecs)/1000;
    asm volatile ("msr cntp_tval_el0, %0" :: "r"(interval));
}

void timer_enable() {
    uint64_t val = 1;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(val));
    asm volatile ("msr cntkctl_el1, %0" :: "r"(val));
}

void permanent_disable_timer(){
    uint64_t ctl = 0;
    asm volatile ("msr cntp_ctl_el0, %0" :: "r"(ctl));
}

void timer_init(uint64_t msecs) {
    _msecs = msecs;
    timer_reset();
    timer_enable();
}

void virtual_timer_reset(uint64_t smsecs) {
    uint64_t freq;
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t interval = (freq * smsecs)/1000;
    asm volatile ("msr cntv_tval_el0, %0" :: "r"(interval));
}

void virtual_timer_enable() {
    uint64_t val = 1;
    asm volatile ("msr cntv_ctl_el0, %0" :: "r"(val));
}

uint64_t virtual_timer_remaining_msec() {
    uint64_t ticks;
    uint64_t freq;
    asm volatile ("mrs %0, cntv_tval_el0" : "=r"(ticks));
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    return (ticks * 1000) / freq;
}

uint64_t timer_now() {
    uint64_t val;
    asm volatile ("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

uint64_t timer_now_msec() {
    uint64_t ticks, freq;
    asm volatile ("mrs %0, cntpct_el0" : "=r"(ticks));
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    return (ticks * 1000) / freq;
}