#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "process/process.h"

process_t *create_kernel_process(const char *name, int (*func)(int argc, char* argv[]), int argc, const char* argv[]);

#ifdef __cplusplus
}
#endif