#include "syscalls.h"
#include "std/string.h"

void printf(const char *fmt, ...){
    __attribute__((aligned(16))) va_list args;
    va_start(args, fmt);
    string str = string_format_va(fmt, args);
    va_end(args);
    printl(str.data);
    free(str.data, str.mem_length);
}