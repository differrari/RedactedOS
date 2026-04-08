#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "process/process.h"
#include "std/string.h"

process_t* load_elf_file(const char *name, const char *bundle, void* file, size_t filesize);
process_t* load_elf_process_path(const char *name, const char *bundle, const char *path, int argc, const char *argv[]);
bool setup_process_args(process_t *proc, int argc, const char *argv[]);
void get_elf_debug_info(process_t* proc, void* file, size_t filesize);

#ifdef __cplusplus
}
#endif