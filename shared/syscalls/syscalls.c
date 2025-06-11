#include "syscalls.h"
#include "std/string.h"

void printf(const char *fmt, ...){
    va_list args;
    string str = string_format(fmt, args);
    printl(str.data);
    free(str.data, str.mem_length);
}