#pragma once

#include "process/process.h"

#ifdef __cplusplus
extern "C" {
#endif

process_t* create_cat_process(int argc, const char *argv[]);

#ifdef __cplusplus
}
#endif
