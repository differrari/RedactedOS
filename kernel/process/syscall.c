#include "syscall.h"
#include "console/kio.h"
#include "exception_handler.h"
#include "console/serial/uart.h"
#include "gic.h"

void sync_el0_handler_c(){
    //We'll need to inspect this call is what we think it is
    //We'll need to check if this is allowed, since not all syscalls are allowed from processes

    const char* x0;
    asm volatile ("mov %0, x0" : "=r"(x0));
    uint64_t *x1;
    asm volatile ("mov %0, x1" : "=r"(x1));
    uint64_t x2;
    asm volatile ("mov %0, x2" : "=r"(x2));
    uint64_t x8;
    asm volatile ("mov %0, x8" : "=r"(x8));

    uint64_t esr;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));

    uint64_t elr;
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));

    uint64_t spsr;
    asm volatile ("mrs %0, spsr_el1" : "=r"(spsr));

    if (x8 == 3) {
        kprintf_args_raw(x0, x1, x2);
        asm volatile ("msr elr_el1, %0" : : "r"(elr));
    } else {
        handle_exception("UNEXPECTED EL0 EXCEPTION");
    }

    asm volatile ("msr spsr_el1, %0" : : "r"(spsr)); // Restore user's SPSR
    asm volatile ("eret");
}