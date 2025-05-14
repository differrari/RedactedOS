#pragma once

#include "types.h"
#include "process.h"

#ifdef __cplusplus
extern "C" {
#endif
process_t* create_process(char *name, void (*func)(), uint64_t code_size, uint64_t func_base, void* data, uint64_t data_size);
#ifdef __cplusplus
}
#endif