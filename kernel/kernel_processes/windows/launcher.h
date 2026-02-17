#pragma once

#include "process/process.h"
#include "string/slice.h"
#include "process/loading/package_info.h"

typedef struct {
    string_slice name;
    string_slice ext;
    string path;
    string file_name;
    package_info info;
} launch_entry;

void load_entries();
void draw_tile(uint32_t column, uint32_t row);
bool await_gpu();
void draw_full();
package_info get_pkg_info(char* info_path);
void activate_current();
uint16_t find_extension(char *path);
void handle_entry(const char *directory, const char *file);
#ifdef __cplusplus
extern "C"{
#endif
process_t* launch_launcher();
#ifdef __cplusplus
}
#endif
