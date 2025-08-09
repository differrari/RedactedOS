#include "async.h"
#include "console/kio.h"

void delay(uint32_t ms) {
    uint64_t freq;
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));

    uint64_t ticks;
    asm volatile ("mrs %0, cntvct_el0" : "=r"(ticks));

    uint64_t target = ticks + (freq / 1000) * ms;

    while (1) {
        uint64_t now;
        asm volatile ("mrs %0, cntvct_el0" : "=r"(now));
        if (now >= target) break;
    }
}

#define WAIT_COND (*reg & expected_value) == expected_value
#define WAIT_CHECK (match > 0) ^ condition

bool wait(uint32_t *reg, uint32_t expected_value, bool match, uint32_t timeout){
    bool condition = WAIT_COND;
    while (WAIT_CHECK) {
        if (timeout != 0){
            timeout--;
#if QEMU
            delay(0);
#else
            //TODO: should be possible to make the delay shorter with some extra math
            delay(1);
#endif
        }
        condition = WAIT_COND;
        if (timeout == 0)
            return WAIT_CHECK;
    }

    return true;
}