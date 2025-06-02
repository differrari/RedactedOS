#include "types.h"

bool ef_init();
void ef_read_root(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index);
void ef_read_dump(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index);