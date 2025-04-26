#include "kio.h"
#include "serial/uart.h"
#include "string.h"
#include "gic.h"
#include "kconsole/kconsole.h"

static bool use_visual = true;

void puts(const char *s){
    uart_puts(s);
    if (use_visual)
        kconsole_puts(s);
}

void putc(const char c){
    uart_putc(c);
    if (use_visual)
        kconsole_putc(c);
}

void printf_args(const char *fmt, const uint64_t *args, uint32_t arg_count){
    string s = string_format_args(fmt, args, arg_count);
    puts(s.data);
    putc('\n');
}

void disable_visual(){
    use_visual = false;
}

void enable_visual(){
    use_visual = true;
}