#pragma once

#include "types.h"
#include "std/string.h"

#ifdef __cplusplus
extern "C" {
#endif

void* read_file(char *path, size_t size);
string_list* list_directory_contents(char *path);
bool init_boot_filesystem();
bool init_dev_filesystem();

#ifdef __cplusplus
}
#endif