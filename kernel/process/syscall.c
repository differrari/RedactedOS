#include "syscall.h"
#include "console/kio.h"
#include "exception_handler.h"
#include "console/serial/uart.h"
#include "gic.h"
#include "ram_e.h"

static uint64_t syscall_sp = 0;

void sync_el0_handler_c(){
    uint64_t x0;
    asm volatile ("mov %0, x11" : "=r"(x0));
    uint64_t *x1;
    asm volatile ("mov %0, x12" : "=r"(x1));
    uint64_t x2;
    asm volatile ("mov %0, x13" : "=r"(x2));
    uint64_t x8;
    asm volatile ("mov %0, x14" : "=r"(x8));
    uint64_t x29;
    asm volatile ("mov %0, x15" : "=r"(x29));
    uint64_t x30;
    asm volatile ("mov %0, x16" : "=r"(x30));
    uint64_t elr;
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    uint64_t spsr;
    asm volatile ("mrs %0, spsr_el1" : "=r"(spsr));
    uint64_t sp_el0;
    asm volatile ("mrs %0, sp_el0" : "=r"(sp_el0));

    uint64_t sp;
    asm volatile ("mov %0, sp" : "=r"(sp));
    
    if (x8 == 3) {
        kprintf_args_raw((const char *)x0, x1, x2);
    } else {
        handle_exception("UNEXPECTED EL0 EXCEPTION");
    }
            
    asm volatile ("mov x29, %0" :: "r"(x29));
    asm volatile ("mov x30, %0" :: "r"(x30));
    asm volatile ("msr sp_el0, %0" :: "r"(sp_el0));
    asm volatile ("msr elr_el1, %0" : : "r"(elr));
    asm volatile ("msr spsr_el1, %0" : : "r"(spsr));
    asm volatile ("mov sp, %0" :: "r"(sp_el0));
    asm volatile ("eret");
    
}