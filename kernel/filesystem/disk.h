#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "std/string.h"

bool init_disk_device();
void disk_verbose();
bool init_boot_filesystem();

void disk_write(const void *buffer, uint32_t sector, uint32_t count);
void disk_read(void *buffer, uint32_t sector, uint32_t count);

void* read_file(char *path);
string_list* list_directory_contents(char *path);

#ifdef __cplusplus
}
#endif