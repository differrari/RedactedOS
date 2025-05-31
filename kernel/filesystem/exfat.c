#include "exfat.h"
#include "disk.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "std/string.h"

void *fs_page;

typedef struct exfat_mbs {
    uint8_t jumpboot[3];//3
    char fsname[8];//8
    uint8_t mustbezero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t first_cluster_of_root_directory;
    uint32_t volume_serial_number;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t bytes_per_sector_shift;
    uint8_t sectors_per_cluster_shift;
    uint8_t number_of_fats;
    uint8_t drive_select;
    uint8_t percent_in_use;
    uint8_t rsvd[7];
    char bootcode[390];
    uint16_t bootsignature;
}__attribute__((packed)) exfat_mbs;

bool ef_init(){
    fs_page = alloc_page(0x1000, true, true, false);

    uint32_t count = 1;

    char *buffer = allocate_in_page(fs_page, count * 512, ALIGN_64B, true, true);
    
    disk_read((void*)buffer, 1, count);

    exfat_mbs* mbs = (exfat_mbs*)buffer;

    if (mbs->bootsignature != 0xAA55){
        kprintf("Wrong boot signature %h",mbs->bootsignature);
        return false;
    }
    if (strcont("EXFAT", mbs->fsname) != 0){
        kprintf("Wrong filesystem type %s",(uintptr_t)mbs->fsname);
        return false;
    }
    uintptr_t extended = (1 << mbs->bytes_per_sector_shift) - 512;
    if (extended > 0){
        kprintf("[exfat implementation error] we don't support extended boot sector yet");
        return false;
    }

    //We'll use (1 << mbs->bytes_per_sector_shift) - 512 to find the size of the extended boot sector and retrieve that too

    for (uint64_t i = 0; i < count * 512; i++){
        if (i % 8 == 0){ puts("\n["); puthex(i/8); puts("]: "); }
        char c = buffer[i];
        if (c >= 0x20 && c <= 0x7E) putc(c);
        else puthex(c);
    }
    putc('\n');
}