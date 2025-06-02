#pragma once

#include "types.h"
#include "process/process.h"

#ifdef __cplusplus
extern "C" {
#endif
process_t* create_process(char *name, void *content, uint64_t content_size, uintptr_t entry);
void translate_enable_verbose();
#ifdef __cplusplus
}
#endif