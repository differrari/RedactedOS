#pragma once

#include "types.h"
#include "process/process.h"

process_t* create_process(void (*func)(), uint64_t code_size, uint64_t func_base, void* data, uint64_t data_size);