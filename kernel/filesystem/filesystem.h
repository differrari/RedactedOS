#pragma once

#include "types.h"
#include "std/string.h"

#ifdef __cplusplus
extern "C" {
#endif

void* read_file(char *path);
string_list* list_directory_contents(char *path);
bool init_boot_filesystem();

#ifdef __cplusplus
}
#endif