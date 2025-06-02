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
        uint8_t entry_type;
        uint8_t entry_count;
        uint16_t checksum;
        uint16_t flags;
        
        // uint8_t custom[14];
        uint16_t rsvd;
        uint32_t create_timestamp;
        uint32_t last_modified;
        uint32_t last_accessed;
        uint8_t create10msincrement;
        uint8_t lastmod10msincrement;
        uint8_t createutcoffset;
        uint8_t lastmodutcoffset;
        uint8_t lastaccutcoffset;
        
        uint8_t rsvd2[7];
}__attribute__((packed)) file_entry;

typedef struct fileinfo_entry {
        uint8_t entry_type;
        uint8_t flags;
        uint8_t rsvd;
        uint8_t name_length;
        uint16_t name_hash;
        uint16_t rsvd2;
        uint64_t valid_filesize;
        uint32_t rsvd3;
        uint32_t first_cluster;
        uint64_t filesize;
}__attribute__((packed)) fileinfo_entry;

typedef struct filename_entry {
        uint8_t entry_type;
        uint8_t flags;
        uint16_t name[15];
}__attribute__((packed)) filename_entry;

void* ef_read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t root_index){
    uint32_t count = cluster_size;

    kprintf("Reading cluster %i, starting from %i (LBA %i)", root_index, cluster_start, cluster_start + ((root_index - 2) * cluster_size));

    void* buffer = (char*)allocate_in_page(fs_page, cluster_size * 512, ALIGN_64B, true, true);
    
    disk_read(buffer, cluster_start + ((root_index - 2) * cluster_size), count);
    
    return buffer;
}

void ef_read_root(uint32_t cluster_start, uint32_t cluster_size, uint32_t root_index){

    char *buffer = (char*)ef_read_cluster(cluster_start, cluster_size, root_index);

    file_entry *entry;
    fileinfo_entry *entry1;
    filename_entry *entry2;
    for (uint64_t i = 0; i < cluster_size * 512; i++){
        char c = buffer[i];
        if (c == 0x85){
            entry = (file_entry *)&buffer[i];
            // kprintf("Found entry with %i more subsequent entries", entry->entry_count);
            i += sizeof(file_entry)-1;
        }
        else if (c == 0xC0){
            entry1 = (fileinfo_entry *)&buffer[i];
            i += sizeof(fileinfo_entry)-1;
        }
        else if (c == 0xC1){
            entry2 = (filename_entry *)&buffer[i];
            char name[15];
            utf16tochar(entry2->name, name, 15);
            kprintf("%s - %i -> %h", (uintptr_t)name, entry1->filesize, entry1->first_cluster);
            if (strcmp(name, "root") == 0){
                uint32_t filecluster = entry1->first_cluster;
                kprintf("Found root at %h - %h bytes", filecluster,entry1->filesize);
                ef_read_root(cluster_start, cluster_size, filecluster);
            }
            if (strcmp(name, "hello.txt") == 0){
                uint32_t filecluster = entry1->first_cluster;
                kprintf("Found hello.txt at %h - %h bytes", filecluster,entry1->filesize);
                ef_read_dump(cluster_start, cluster_size, filecluster);
            }
            i += sizeof(filename_entry)-1;
        }
        // else if (c >= 0x20 && c <= 0x7E) putc(c);
        // else puthex(c);
    }
    // puts("Done printing");
    putc('\n');
}

void ef_read_dump(uint32_t cluster_start, uint32_t cluster_size, uint32_t root_index){

    char *buffer = (char*)ef_read_cluster(cluster_start, cluster_size, root_index);

    for (uint64_t i = 0; i < cluster_size * 512; i++){
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
    
    disk_read((void*)mbs, 0, 1);

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
        // return false;
    }

    if (mbs->first_cluster_of_root_directory > mbs->cluster_count){
        kprintf("[exfat error] root directory cluster not found");
        return false;
    }

    kprintf("EXFAT Volume uses %i cluster size", 1 << mbs->bytes_per_sector_shift);

    kprintf("Cluster at %h (%h * %h of size %h each)",mbs->fat_offset + mbs->fat_length * mbs->number_of_fats, mbs->cluster_heap_offset,mbs->cluster_count, 1 << mbs->sectors_per_cluster_shift);    
    ef_read_FAT(mbs->fat_offset, mbs->fat_length, mbs->number_of_fats);
    ef_read_root(mbs->cluster_heap_offset, 1 << mbs->sectors_per_cluster_shift, mbs->first_cluster_of_root_directory);

    // while(1);
    return true;
}