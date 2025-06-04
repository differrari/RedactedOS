#include "syscalls.h"
#include "std/string.h"

void printf_args(const char *fmt, const uint64_t *args, uint32_t arg_count){
    string str = string_format_args(fmt, args, arg_count);
    printl(str.data);
    free(str.data, str.mem_length);
}