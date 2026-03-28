#pragma once

#include "process/process.h"

#ifdef __cplusplus
extern "C" {
#endif

process_t* execute(const char* prog_name, int argc, const char* argv[]);

#ifdef __cplusplus
}
#endif

extern system_module bin_module;