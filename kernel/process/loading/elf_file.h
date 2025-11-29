#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "process/process.h"
#include "std/string.h"

process_t* load_elf_file(const char *name, const char *bundle, void* file, size_t filesize);
void get_elf_debug_info(process_t* proc, void* file, size_t filesize);

#ifdef __cplusplus
}
#endif