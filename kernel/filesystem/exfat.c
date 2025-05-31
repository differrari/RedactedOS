#include "exfat.h"
#include "disk.h"
#include "memory/page_allocator.h"
#include "console/kio.h"

void *fs_page;

void ef_read_test_file(){
    fs_page = alloc_page(0x1000, true, true, false);

    uint32_t count = 1;

    char *buffer = allocate_in_page(fs_page, count * 512, ALIGN_64B, true, true);
    
    disk_read((void*)buffer, 1, count);

    for (uint64_t i = 0; i < count * 512; i++){
        if (i % 8 == 0){ puts("\n["); puthex(i/8); puts("]: "); }
        char c = buffer[i];
        if (c >= 0x20 && c <= 0x7E) putc(c);
        else puthex(c);
    }
    putc('\n');
}