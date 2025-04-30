#pragma once

#include "types.h"
#include "process.h"

process_t *create_kernel_process(void (*func)());