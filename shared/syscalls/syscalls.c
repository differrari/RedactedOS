#include "syscalls.h"

void printf_args(const char *fmt, const uint64_t *args, uint32_t arg_count){
    register uint64_t x8 asm("x8") = PRINTF_SYSCALL;

    asm volatile(
        "svc #3"
        :
        : "r"(x8)
        : "memory"
    );
}