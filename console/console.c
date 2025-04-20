#include "console.h"
#include "serial/uart.h"
#include "string.h"

void puts(const char *s){
    uart_puts(s);
}

void putc(const char c){
    uart_putc(c);
}

void puthex(uint64_t value){
    uart_puthex(value);
}

void printf_args(const char *fmt, const uint64_t *args, uint32_t arg_count){
    string s = string_format_args(fmt, args, arg_count);
    puts(s.data);
    putc('\n');
}