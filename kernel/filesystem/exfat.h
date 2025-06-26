#include "types.h"
#include "std/string.h"

#ifdef __cplusplus
extern "C" {
#endif
bool ef_init();
void ef_read_dump(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index);
void* ef_read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index);
void* ef_read_file(const char *path);
string_list* ef_list_contents(const char *path);
#ifdef __cplusplus
}
#endif