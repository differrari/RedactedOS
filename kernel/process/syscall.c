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

    // asm volatile ("mrs %0, esr_el1" : "=r"(esr));
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));

    // uint32_t ec = (esr >> 26) & 0x3F;

    // asm volatile ("mrs %0, far_el1" : "=r"(far));
    // elr -= 4;
    // uint32_t instr = *(uint32_t *)elr;
    
    // uint8_t svc_num = (instr >> 5) & 0xFFFF;
    uart_raw_puts("Hello");
    // uart_puts("Hello");
    
    if (x8 == 3){
        printf_args_raw(x0,x1,x2);
    }

    asm volatile ("eret");

    // while (1){}

    // asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    // asm volatile ("mrs %0, far_el1" : "=r"(far));
    // elr -= 4;
    // uint32_t instr = *(uint32_t *)elr;
    // uint8_t svc_num = (instr >> 5) & 0xFFFF;
    // printf("Call is %i, from %h [%h]. X0: %h",svc_num, elr, instr,x0);
    // if (svc_num == 3){
    //     // register const char *msg asm("x0");
    //     // uart_puts(msg);
    //     printf("We'll print here");
    // }
    // while (1)
    // {}
}