#pragma once 

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct partition_entry
{
    uint8_t status;
    uint8_t first_sector_chs[3];
    uint8_t type;
    uint8_t last_sector_chs[3];
    uint32_t first_sector;
    uint32_t num_sectors;
}__attribute__((packed)) partition_entry;

typedef struct mbr {
    uint8_t bootstrap[0x1BE];
    partition_entry partitions[4];
    uint16_t signature;
}__attribute__((packed, aligned(1))) mbr;

uint32_t mbr_find_partition(uint8_t partition_type);

#ifdef __cplusplus
}
#endif