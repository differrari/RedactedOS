#pragma once

#include "process.h"

void proc_func();

#ifdef __cplusplus
extern "C" {
#endif
process_t* default_processes();
#ifdef __cplusplus
}
#endif