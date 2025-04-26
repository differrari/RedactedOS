#include "syscall.h"
#include "console/kio.h"
#include "exception_handler.h"
#include "console/serial/uart.h"

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
    
    uint64_t esr, elr, far;

    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    asm volatile ("mrs %0, daif" : "=r"(far));

    printf_raw("IRQ STATUS %h",far);
    
    if (x8 == 3){
        printf_args_raw(x0,x1,x2);
    } else {
        handle_exception("UNEXPECTED EL0 EXCEPTION");
    }

    asm volatile ("eret");
}