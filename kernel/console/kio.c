#include "kio.h"
#include "serial/uart.h"
#include "kstring.h"
#include "interrupts/irq.h"
#include "memory/kalloc.h"
#include "kconsole/kconsole.h"

static bool use_visual = true;

void puts(const char *s){
    uart_raw_puts(s);
    if (use_visual)
        kconsole_puts(s);
}

void putc(const char c){
    uart_raw_putc(c);
    if (use_visual)
        kconsole_putc(c);
}

void puthex(uint64_t value){
    uart_puthex(value);
    if (use_visual)
        kconsole_puthex(value);
}

void kprintf_args(const char *fmt, const uint64_t *args, uint32_t arg_count){
    kprintf_args_raw(fmt, args, arg_count);
}

void kprintf_args_raw(const char *fmt, const uint64_t *args, uint32_t arg_count){
    kstring s = kstring_format_args(fmt, args, arg_count);
    puts(s.data);
    putc('\n');
    temp_free(s.data,256);
}

void kprintf_l(const char *fmt){
    puts(fmt);
    putc('\n');
}

void kputf_args_raw(const char *fmt, const uint64_t *args, uint32_t arg_count){
    kstring s = kstring_format_args(fmt, args, arg_count);
    puts(s.data);
    temp_free(s.data,256);
}

void disable_visual(){
    use_visual = false;
}

void enable_visual(){
    use_visual = true;
}