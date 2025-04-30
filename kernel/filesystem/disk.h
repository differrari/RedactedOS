#pragma once

#include "types.h"
#include "dtb.h"

void find_disk();
bool init_disk();
uint64_t get_disk_address();
uint64_t get_disk_size();