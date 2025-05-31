#include "types.h"

bool find_disk();
void disk_verbose();
void disk_init_test();

void disk_write(const void *buffer, uint32_t sector, uint32_t count);
void disk_read(void *buffer, uint32_t sector, uint32_t count);