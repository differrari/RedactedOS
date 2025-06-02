#include "types.h"

bool ef_init();
void* ef_read_directory(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index, const char *seek);
void ef_read_dump(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index);
void* ef_read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index);
void* ef_read_file(const char *path);