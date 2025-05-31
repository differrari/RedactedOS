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

typedef struct file_entry {
        uint8_t entry_type;// = 0x85;
        uint8_t entry_count; // This is the number of subsequent entries that also belong to this "file" entry. Should be at least 2, one 0xC0 and one 0xC1 for info and filename.
        uint16_t rsvd;
        uint32_t flags; // 0x10 == directory, probably identical to fat32
        uint32_t creation, modification, access;
        char pad[12];
}__attribute__((packed)) file_entry;

typedef struct fileinfo_entry {
        uint8_t entryType;
        uint8_t flags;
        uint8_t unk; // = 0. May be part of next field, but then it'd be in big-endian order which is unlikely.
        uint8_t filenameLengthInBytes;
        uint64_t filesize;
        uint32_t unk2;
        uint32_t startCluster; // minus 2! Same as fat12/16/32.
        uint64_t filesize2;
}__attribute__((packed)) fileinfo_entry;

typedef struct filename_entry {
        uint8_t entrytype;
        uint8_t entrycount;
        uint16_t name[15];
}__attribute__((packed)) filename_entry;

void ef_read_root(uint32_t cluster_start, uint32_t cluster_size, uint32_t root_index){

    uint32_t count = cluster_size;

    kprintf("Reading cluster %i (LBA %i)", root_index, cluster_start + ((root_index - 2) * cluster_size));

    char* buffer = (char*)allocate_in_page(fs_page, cluster_size * 512, ALIGN_64B, true, true);
    
    disk_read((void*)buffer, cluster_start + ((root_index - 2) * cluster_size), count);

    for (uint64_t i = 0; i < count * 512; i++){
        char c = buffer[i];
        if (c == 0x85){
            file_entry *entry = (file_entry *)&buffer[i];
            kprintf("Found entry with %i more subsequent entries", entry->entry_count);
            i += sizeof(file_entry)-1;
        }
        else if (c == 0xC0){
            fileinfo_entry *entry1 = (fileinfo_entry *)&buffer[i];
            kprintf("Found info entry with %i size", entry1->filenameLengthInBytes);
            i += sizeof(fileinfo_entry)-1;
        }
        else if (c == 0xC1){
            filename_entry *entry2 = (filename_entry *)&buffer[i];
            char name[15];
            utf16tochar(entry2->name, name, 15);
            kprintf("Found name entry: %s", (uintptr_t)name);
            i += sizeof(filename_entry)-1;
        }
        else if (c >= 0x20 && c <= 0x7E) putc(c);
    }
    // puts("Done printing");
    putc('\n');
}

void ef_read_debug(uint32_t cluster_start, uint32_t cluster_size, uint32_t root_index){

    uint32_t count = cluster_size;

    kprintf("Reading cluster %i (LBA %i)", root_index, cluster_start + ((root_index - 2) * cluster_size));

    char* buffer = (char*)allocate_in_page(fs_page, cluster_size * 512, ALIGN_64B, true, true);
    
    disk_read((void*)buffer, cluster_start + ((root_index - 2) * cluster_size), count);

    for (uint64_t i = 0; i < count * 512; i++){
        // if (i % 8 == 0){ puts("\n["); puthex(i/8); puts("]: "); }
        char c = buffer[i];
        if (c >= 0x20 && c <= 0x7E) putc(c);
        // else puthex(c);
    }
    // puts("Done printing");
    putc('\n');
}

void ef_read_FAT(uint32_t location, uint32_t size, uint8_t count){
    uint32_t* fat = (uint32_t*)allocate_in_page(fs_page, size * count * 512, ALIGN_64B, true, true);
    disk_read((void*)fat, location, size);
    kprintf("FAT: %h (%h)",location*512,size * count * 512);
    uint32_t total_entries = (size * count * 512) / 4;
    for (uint32_t i = 0; i < total_entries; i++)
        if (fat[i] != 0) kprintf("[%i] = %h", i, fat[i]);
}

bool ef_init(){
    fs_page = alloc_page(0x1000, true, true, false);

    exfat_mbs* mbs = (exfat_mbs*)allocate_in_page(fs_page, 512, ALIGN_64B, true, true);
    
    disk_read((void*)mbs, 1, 1);

    if (mbs->bootsignature != 0xAA55){
        kprintf("[exfat] Wrong boot signature %h",mbs->bootsignature);
        return false;
    }
    if (strcmp("EXFAT   ", mbs->fsname) != 0){
        kprintf("[exfat error] Wrong filesystem type %s",(uintptr_t)mbs->fsname);
        return false;
    }
    uintptr_t extended = (1 << mbs->bytes_per_sector_shift) - 512;
    if (extended > 0){
        kprintf("[exfat implementation error] we don't support extended boot sector yet");
        return false;
    }

    if (mbs->first_cluster_of_root_directory > mbs->cluster_count){
        kprintf("[exfat error] root directory cluster not found");
        return false;
    }
    kprintf("Cluster at %h (%h size %i)",mbs->fat_offset + mbs->fat_length * mbs->number_of_fats, mbs->cluster_heap_offset,mbs->cluster_count);    
    ef_read_FAT(mbs->fat_offset, mbs->fat_length, mbs->number_of_fats);
    ef_read_root(mbs->cluster_heap_offset, 1 << mbs->sectors_per_cluster_shift, mbs->first_cluster_of_root_directory);

    while(1);
    return true;
}