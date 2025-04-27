#include "syscall.h"
#include "console/kio.h"
#include "exception_handler.h"
#include "console/serial/uart.h"
#include "gic.h"

void sync_el0_handler_c(){
    asm volatile ("msr daifset, #2");
    
    uint64_t x0;
    asm volatile ("mov %0, x0" : "=r"(x0));
    uint64_t *x1;
    asm volatile ("mov %0, x1" : "=r"(x1));
    uint64_t x2;
    asm volatile ("mov %0, x2" : "=r"(x2));
    uint64_t x8;
    asm volatile ("mov %0, x8" : "=r"(x8));
    uint64_t x29;
    asm volatile ("mov %0, x29" : "=r"(x29));
    uint64_t x30;
    asm volatile ("mov %0, x30" : "=r"(x30));
    uint64_t elr;
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    
    uint64_t spsr;
    asm volatile ("mrs %0, spsr_el1" : "=r"(spsr));
    
    if (x8 == 3) {
        kprintf_args_raw((const char *)x0, x1, x2);
    } else {
        handle_exception("UNEXPECTED EL0 EXCEPTION");
    }
            
    asm volatile ("mov x29, %0" :: "r"(x29));
    asm volatile ("mov x30, %0" :: "r"(x30));
    asm volatile ("msr elr_el1, %0" : : "r"(elr));
    asm volatile ("msr spsr_el1, %0" : : "r"(spsr));
    asm volatile ("eret");
    
}