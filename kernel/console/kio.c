#include "kio.h"
#include "serial/uart.h"
#include "kconsole/kconsole.h"
#include "std/string.h"
#include "memory/page_allocator.h"

static bool use_visual = true;
void* print_buf;

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

void init_print_buf(){
    print_buf = palloc(0x1000,true, false, false);
}

void kprintf(const char *fmt, ...){
    if (!print_buf) init_print_buf();
    va_list args;
    va_start(args, fmt);
    char* buf = kalloc(print_buf, 256, ALIGN_64B, true, false);
    size_t len = string_format_va_buf(fmt, buf, args);
    va_end(args);
    puts(buf);
    putc('\r');
    putc('\n');
    kfree((void*)buf, 256);
}

void kprint(const char *fmt){
    puts(fmt);
    putc('\r');
    putc('\n');
}

void kputf(const char *fmt, ...){
    if (!print_buf) init_print_buf();
    va_list args;
    va_start(args, fmt);
    char* buf = kalloc(print_buf, 256, ALIGN_64B, true, false);
    size_t len = string_format_va_buf(fmt, buf, args);
    va_end(args);
    puts(buf);
    kfree((void*)buf, 256);
}

void disable_visual(){
    use_visual = false;
}

void enable_visual(){
    use_visual = true;
}