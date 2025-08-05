#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "types.h"
#include "process/process.h"

process_t* create_process(const char *name, void *content, uint64_t content_size, uintptr_t entry);
void translate_enable_verbose();
#ifdef __cplusplus
}
#endif