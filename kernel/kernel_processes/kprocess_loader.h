#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "process.h"

process_t *create_kernel_process(char *name, void (*func)());

#ifdef __cplusplus
}
#endif