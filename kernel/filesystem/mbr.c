#include "mbr.h"
#include "memory/page_allocator.h"
#include "memory/memory_access.h"
#include "disk.h"
#include "console/kio.h"

void* mbr_page;

uint32_t mbr_find_partition(uint8_t partition_type){
    mbr_page = palloc(0x1000, true, true, false);

    mbr *mbr_entry = (mbr*)kalloc(mbr_page, 512, ALIGN_64B, true, true);
    
    disk_read((void*)mbr_entry, 0, 1);

    uint32_t offset = 0;

    for (uint8_t i = 0; i < 4; i++){
        partition_entry *entry = &mbr_entry->partitions[i];
        kprintf("MBR Partition %i = %x -> %x",i, entry->type, read_unaligned32(&entry->first_sector));
        if (entry->type == partition_type){
            offset = read_unaligned32(&entry->first_sector);
        }
    }

    return offset;
}