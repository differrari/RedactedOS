#include "exception_handler.h"
#include "console/serial/uart.h"
#include "console/kio.h"
#include "graph/graphics.h"
#include "timer.h"
#include "theme/theme.h"
#include "std/string.h"

static bool panic_triggered = false;

void set_exception_vectors(){
    extern char exception_vectors[];
    asm volatile ("msr vbar_el1, %0" :: "r"(exception_vectors));
}

void handle_exception(const char* type, uint64_t info) {
    uint64_t esr, elr, far;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    asm volatile ("mrs %0, far_el1" : "=r"(far));

    disable_visual();//Disable visual kprintf, since it has additional memory accesses that could be faulting

    string s = string_format("%s \r\nESR_EL1: %x\r\nELR_EL1: %x\r\nFAR_EL1: %x",(uintptr_t)type,esr,elr,far);
    panic(s.data, info);
}

void fiq_el1_handler(){ handle_exception("FIQ EXCEPTION\r\n", 0); }

void error_el1_handler(){ handle_exception("ERROR EXCEPTION\r\n", 0); }

void draw_panic_screen(string s){
    gpu_clear(system_theme.err_color);
    uint32_t scale = 3;
    gpu_draw_string(*(string *)&s, (gpu_point){20,20}, scale, 0xFFFFFFFF);
    gpu_flush();
}

void panic(const char* msg, uint64_t info) {
    permanent_disable_timer();

    uint64_t esr, elr, far;
    asm volatile ("mrs %0, esr_el1" : "=r"(esr));
    asm volatile ("mrs %0, elr_el1" : "=r"(elr));
    asm volatile ("mrs %0, far_el1" : "=r"(far));

    bool old_panic_triggered = panic_triggered;
    panic_triggered = true;
    uart_raw_puts("*** ");
    uart_raw_puts(system_config.panic_text);
    uart_raw_puts(" ***\r\n");
    uart_raw_puts(msg);
    uart_raw_puts("\r\n");
    uart_raw_puts("Additional info: ");
    uart_puthex(info);
    uart_raw_puts("\r\n");
    uart_raw_puts("System Halted\r\n");
    if (!old_panic_triggered){
        string s = string_format("%s\r\n%s\r\nError code: %x\r\nSystem Halted",(uint64_t)system_config.panic_text,(uint64_t)msg,info);
        draw_panic_screen(s);
    }
    while (1);
}