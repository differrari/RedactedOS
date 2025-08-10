#pragma once

#include "process/process.h"

#ifdef __cplusplus
extern "C" {
#endif

process_t* create_cat_process(const char *args);

#ifdef __cplusplus
}
#endif
