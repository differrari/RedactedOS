#include "syscalls.h"
#include "std/string.h"

void printf(const char *fmt, ...){
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);
    char li[256]; 
    string_format_va_buf(fmt, li, args);
    va_end(args);
    printl(li);
}