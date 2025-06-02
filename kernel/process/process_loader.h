#pragma once

#include "types.h"
#include "process/process.h"

#ifdef __cplusplus
extern "C" {
#endif
process_t* create_process(char *name, void (*func)(), uint64_t code_size, uint64_t func_base, void* data, uint64_t data_size);
void translate_enable_verbose();
#ifdef __cplusplus
}
#endif