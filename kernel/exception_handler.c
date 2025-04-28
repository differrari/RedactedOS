#include "exception_handler.h"
#include "console/serial/uart.h"
#include "kstring.h"
#include "console/kio.h"
#include "mmu.h"

void set_exception_vectors(){
    extern char exception_vectors[];
    kprintf("Exception vectors setup at %h", (uint64_t)&exception_vectors);
    asm volatile ("msr vbar_el1, %0" :: "r"(exception_vectors));
}

void handle_exception(const char* type) {
    uint64_t esr, elr, far;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    asm volatile ("mrs %0, far_el1" : "=r"(far));

    disable_visual();//Disable visual kprintf, since it has additional memory accesses that could be faulting

    kstring s = string_format("%s \nESR_EL1: %h\nELR_EL1: %h\n,FAR_EL1: %h",(uint64_t)string_l(type).data,esr,elr,far);
    panic(s.data);
}

void sync_el1_handler(){ handle_exception("SYNC EXCEPTION");}

void fiq_el1_handler(){ handle_exception("FIQ EXCEPTION\n"); }

void error_el1_handler(){ handle_exception("ERROR EXCEPTION\n"); }

void panic(const char* panic_msg) {
    kprintf_raw("*** KERNEL PANIC ***");
    kprintf_raw(panic_msg);
    kprintf_raw("System Halted");
    while (1);
}

void panic_with_info(const char* msg, uint64_t info) {
    kprintf_raw("*** KERNEL PANIC ***");
    kprintf_raw(msg);
    kprintf_raw("Additional info: %h",info);
    kprintf_raw("System Halted");
    while (1);
}