#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "process/process.h"

process_t *create_kernel_process(const char *name, int (*func)(int argc, char* argv[]), int argc, const char* argv[]);
__attribute__((noreturn)) void kernel_thread_return_trampoline(int32_t exit_code);
__attribute__((noreturn)) void kernel_process_return_trampoline(int32_t exit_code);

#ifdef __cplusplus
}
#endif