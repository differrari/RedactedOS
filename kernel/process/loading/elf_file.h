#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "process/process.h"

process_t* load_elf_file(const char *name, const char *bundle, void* file, size_t filesize);

#ifdef __cplusplus
}
#endif