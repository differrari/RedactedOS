#include "kio.h"
#include "serial/uart.h"
#include "string.h"
#include "kconsole/kconsole.h"

void puts(const char *s){
    uart_raw_puts(s);
    kconsole_puts(s);
}

void putc(const char c){
    uart_raw_putc(c);
    kconsole_putc(c);
}

void printf_args(const char *fmt, const uint64_t *args, uint32_t arg_count){
    asm volatile ("msr daifset, #2");
    string s = string_format_args(fmt, args, arg_count);
    puts(s.data);
    putc('\n');
    asm volatile ("msr daifclr, #2");
}