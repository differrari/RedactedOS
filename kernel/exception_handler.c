#include "exception_handler.h"
#include "console/serial/uart.h"
#include "kstring.h"
#include "console/kio.h"
#include "mmu.h"
#include "graph/graphics.h"

static bool panic_triggered = false;

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

    kstring s = string_format("%s \nESR_EL1: %h\nELR_EL1: %h\nFAR_EL1: %h",(uint64_t)string_l(type).data,esr,elr,far);
    panic(s.data);
}

void sync_el1_handler(){ handle_exception("SYNC EXCEPTION"); }

void fiq_el1_handler(){ handle_exception("FIQ EXCEPTION\n"); }

void error_el1_handler(){ handle_exception("ERROR EXCEPTION\n"); }

void draw_panic_screen(kstring s){
    gpu_clear(0x0000FF);
    uint32_t scale = 3;
    uint32_t size = gpu_get_char_size(3);
    gpu_draw_string(s, (point){20,20}, scale, 0xFFFFFF);
}

void panic(const char* panic_msg) {
    bool old_panic_triggered = panic_triggered;
    panic_triggered = true;
    if (!old_panic_triggered){
        kstring s = string_format("CARDINAL SIN\n%s\nSystem Halted",(uint64_t)panic_msg);
        draw_panic_screen(s);
    }
    uart_raw_puts("*** CARDINAL SIN ***\n");
    uart_raw_puts(panic_msg);
    uart_raw_putc('\n');
    uart_raw_puts("System Halted");
    while (1);
}

void panic_with_info(const char* msg, uint64_t info) {
    bool old_panic_triggered = panic_triggered;
    panic_triggered = true;
    if (!old_panic_triggered){
        kstring s = string_format("CARDINAL SIN\n%s\nError code: %h\nSystem Halted",(uint64_t)msg,info);
        draw_panic_screen(s);
    }
    uart_raw_puts("*** CARDINAL SIN ***\n");
    uart_raw_puts(msg);
    uart_raw_putc('\n');
    uart_raw_puts("Additional info: ");
    uart_puthex(info);
    uart_raw_putc('\n');
    uart_raw_puts("System Halted");
    while (1);
}