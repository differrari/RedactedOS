#include "async.h"
#include "console/kio.h"

void delay(uint32_t ms) {
    uint64_t freq;
    asm volatile ("mrs %0, cntfrq_el0" : "=r"(freq));

    uint64_t ticks;
    asm volatile ("mrs %0, cntpct_el0" : "=r"(ticks));

    uint64_t target = ticks + (freq / 1000) * ms;

    while (1) {
        uint64_t now;
        asm volatile ("mrs %0, cntpct_el0" : "=r"(now));
        if (now >= target) break;
    }
}

bool wait(uint32_t *reg, uint32_t expected_value, bool match, uint32_t timeout){
    bool condition;
    do {
        delay(1);
        timeout--;
        if (timeout == 0){
            kprintf("Wait timed out");
            return false;
        }
        condition = (*reg & expected_value) != 0;
    } while ((match > 0) ^ condition);

    return true;
}