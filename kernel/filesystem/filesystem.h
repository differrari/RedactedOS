#pragma once

#include "types.h"
#include "std/string.h"
#include "dev/driver_base.h"

#ifdef __cplusplus
extern "C" {
#endif

void* read_file(const char *path, size_t size);
sizedptr list_directory_contents(const char *path);
bool init_boot_filesystem();

#ifdef __cplusplus
}
#endif