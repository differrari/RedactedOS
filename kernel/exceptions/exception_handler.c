#include "exception_handler.h"
#include "console/serial/uart.h"
#include "kstring.h"
#include "console/kio.h"
#include "memory/mmu.h"
#include "graph/graphics.h"
#include "timer.h"
#include "theme/theme.h"
#include "std/string.h"

static bool panic_triggered = false;

void set_exception_vectors(){
    extern char exception_vectors[];
    kprintf("Exception vectors setup at %x", (uint64_t)&exception_vectors);
    asm volatile ("msr vbar_el1, %0" :: "r"(exception_vectors));
}

void handle_exception(const char* type) {
    uint64_t esr, elr, far;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    asm volatile ("mrs %0, far_el1" : "=r"(far));

    disable_visual();//Disable visual kprintf, since it has additional memory accesses that could be faulting

    kstring s = kstring_format("%s \r\nESR_EL1: %x\r\nELR_EL1: %x\r\nFAR_EL1: %x",(uint64_t)kstring_l(type).data,esr,elr,far);
    panic(s.data);
}

void handle_exception_with_info(const char* type, uint64_t info) {
    uint64_t esr, elr, far;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    asm volatile ("mrs %0, far_el1" : "=r"(far));

    disable_visual();//Disable visual kprintf, since it has additional memory accesses that could be faulting

    kstring s = kstring_format("%s \r\nESR_EL1: %x\r\nELR_EL1: %x\r\nFAR_EL1: %x",(uint64_t)kstring_l(type).data,esr,elr,far);
    panic_with_info(s.data, info);
}

void fiq_el1_handler(){ handle_exception("FIQ EXCEPTION\r\n"); }

void error_el1_handler(){ handle_exception("ERROR EXCEPTION\r\n"); }

void draw_panic_screen(kstring s){
    gpu_clear(0x0000FF);
    uint32_t scale = 3;
    gpu_draw_string(*(string *)&s, (gpu_point){20,20}, scale, 0xFFFFFF);
    gpu_flush();
}

void panic(const char* panic_msg) {
    permanent_disable_timer();
    
    bool old_panic_triggered = panic_triggered;
    panic_triggered = true;
    uart_raw_puts("*** ");
    uart_raw_puts(PANIC_TEXT);
    uart_raw_puts(" ***\r\n");
    uart_raw_puts(panic_msg);
    uart_raw_puts("\r\n");
    uart_raw_puts("System Halted\r\n");
    if (!old_panic_triggered){
        kstring s = kstring_format("%s\r\n%s\r\nSystem Halted",(uint64_t)PANIC_TEXT,(uint64_t)panic_msg);
        draw_panic_screen(s);
    }
    while (1);//TODO: OPT
}

void panic_with_info(const char* msg, uint64_t info) {
    permanent_disable_timer();

    uint64_t esr, elr, far;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    asm volatile ("mrs %0, far_el1" : "=r"(far));

    bool old_panic_triggered = panic_triggered;
    panic_triggered = true;
    uart_raw_puts("*** ");
    uart_raw_puts(PANIC_TEXT);
    uart_raw_puts(" ***\r\n");
    uart_raw_puts(msg);
    uart_raw_puts("\r\n");
    uart_raw_puts("Additional info: ");
    uart_puthex(info);
    uart_raw_puts("\r\n");
    uart_raw_puts("System Halted\r\n");
    if (!old_panic_triggered){
        kstring s = kstring_format("%s\r\n%s\r\nError code: %x\r\nSystem Halted",(uint64_t)PANIC_TEXT,(uint64_t)msg,info);
        draw_panic_screen(s);
    }
    while (1);//TODO: OPT
}