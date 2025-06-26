#include "exfat.h"
#include "disk.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "std/string.h"
#include "std/memfunctions.h"

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
        struct {
            uint16_t read_only: 1;
            uint16_t hidden: 1;
            uint16_t system: 1;
            uint16_t rsvd: 1;
            uint16_t directory: 1;
            uint16_t archive: 1;
            uint16_t rsvd2: 10;
        } flags;
        
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

exfat_mbs* mbs;

void* ef_read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index){
    uint32_t count = cluster_count * cluster_size;

    uint32_t lba = cluster_start + ((root_index - 2) * cluster_size);

    kprintf("Reading cluster(s) %i-%i, starting from %i (LBA %i)", root_index, root_index+cluster_count, cluster_start, lba);

    void* buffer = (char*)allocate_in_page(fs_page, cluster_count * cluster_size * 512, ALIGN_64B, true, true);
    
    disk_read(buffer, lba, count);
    
    return buffer;
}

const char* advance_path(const char *path){
    while (*path != '/' && *path != '\0')
        path++;
    path++;
    return path;
}

typedef void* (*ef_entry_handler)(file_entry*, fileinfo_entry*, filename_entry*, const char *seek);

void* ef_walk_directory(uint32_t cluster_count, uint32_t root_index, const char *seek, ef_entry_handler handler) {
    uint32_t cluster_size = 1 << mbs->sectors_per_cluster_shift;
    char *buffer = (char*)ef_read_cluster(mbs->cluster_heap_offset, cluster_size, cluster_count, root_index);
    file_entry *entry = 0;
    fileinfo_entry *entry1 = 0;
    filename_entry *entry2 = 0;

    for (uint64_t i = 0; i < cluster_size * 512; i++) {
        char c = buffer[i];
        if (c == 0x85) {
            entry = (file_entry *)&buffer[i];
            i += sizeof(file_entry) - 1;
        } else if (c == 0xC0) {
            entry1 = (fileinfo_entry *)&buffer[i];
            i += sizeof(fileinfo_entry) - 1;
        } else if (c == 0xC1) {
            entry2 = (filename_entry *)&buffer[i];
            if (entry && entry1 && entry2) {
                void *result = handler(entry, entry1, entry2, seek);
                if (result)
                    return result;
            }
            i += sizeof(filename_entry) - 1;
        }
    }

    return 0;
}

void* ef_list_directory(uint32_t cluster_count, uint32_t root_index) {
    uint32_t cluster_size = 1 << mbs->sectors_per_cluster_shift;
    char *buffer = (char*)ef_read_cluster(mbs->cluster_heap_offset, cluster_size, cluster_count, root_index);
    file_entry *entry = 0;
    fileinfo_entry *entry1 = 0;
    filename_entry *entry2 = 0;
    void *list_buffer = (char*)allocate_in_page(fs_page, 0x1000 * cluster_count, ALIGN_64B, true, true);
    uint32_t len = 0; 
    uint32_t count = 0;

    char *write_ptr = (char*)list_buffer + 4;

    for (uint64_t i = 0; i < cluster_size * 512; i++) {
        char c = buffer[i];
        if (c == 0x85) {
            entry = (file_entry *)&buffer[i];
            i += sizeof(file_entry) - 1;
        } else if (c == 0xC0) {
            entry1 = (fileinfo_entry *)&buffer[i];
            i += sizeof(fileinfo_entry) - 1;
        } else if (c == 0xC1) {
            entry2 = (filename_entry *)&buffer[i];
            if (entry && entry1 && entry2) {
                count++;
                char filename[15];
                utf16tochar(entry2->name, filename, 15);
                for (int j = 0; j < 15 && filename[j]; j++)
                    *write_ptr++ = filename[j];
                *write_ptr++ = '\0';
            }
            i += sizeof(filename_entry) - 1;
        }
    }

    *(uint32_t*)list_buffer = count;

    return list_buffer;
}

void* ef_read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index){

    char *buffer = (char*)ef_read_cluster(cluster_start, cluster_size, cluster_count, root_index);

    void *file = allocate_in_page(fs_page, file_size, ALIGN_64B, true, true);

    memcpy(file, (void*)buffer, file_size);
    
    return file;
}

void ef_read_dump(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index){

    char *buffer = (char*)ef_read_cluster(cluster_start, cluster_size, cluster_count, root_index);

    for (uint64_t i = 0; i < cluster_size * cluster_count * 512; i++){
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
    kprintf("FAT: %x (%x)",location*512,size * count * 512);
    uint32_t total_entries = (size * count * 512) / 4;
    // for (uint32_t i = 0; i < total_entries; i++)
    //     if (fat[i] != 0) kprintf("[%i] = %x", i, fat[i]);
}

bool ef_init(){
    fs_page = alloc_page(0x1000, true, true, false);

    mbs = (exfat_mbs*)allocate_in_page(fs_page, 512, ALIGN_64B, true, true);
    
    disk_read((void*)mbs, 0, 1);

    if (mbs->bootsignature != 0xAA55){
        kprintf("[exfat] Wrong boot signature %x",mbs->bootsignature);
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

    kprintf("Cluster at %x (%x * %x of size %x each)",mbs->fat_offset + mbs->fat_length * mbs->number_of_fats, mbs->cluster_heap_offset,mbs->cluster_count, 1 << mbs->sectors_per_cluster_shift);    
    ef_read_FAT(mbs->fat_offset, mbs->fat_length, mbs->number_of_fats);
    return true;
}

static void* read_entry_handler(file_entry *entry, fileinfo_entry *info, filename_entry *name, const char *seek) {
    char filename[15];
    utf16tochar(name->name, filename, 15);

    if (strstart(seek, filename) != 0)
        return 0;

    uint32_t filecluster = info->first_cluster;
    uint32_t bps = 1 << mbs->bytes_per_sector_shift;
    uint32_t spc = 1 << mbs->sectors_per_cluster_shift;
    uint32_t bpc = bps * spc;
    uint32_t count = (info->filesize + bpc - 1) / bpc;

    return entry->flags.directory
        ? ef_walk_directory(count, filecluster, advance_path(seek), read_entry_handler)
        : ef_read_full_file(mbs->cluster_heap_offset, 1 << mbs->sectors_per_cluster_shift, count, info->filesize, filecluster);
}

void* ef_read_file(const char *path){
    path = advance_path(path);

    return ef_walk_directory(1, mbs->first_cluster_of_root_directory, path, read_entry_handler);
}

static void* list_entries_handler(file_entry *entry, fileinfo_entry *info, filename_entry *name, const char *seek) {
    char filename[15];
    utf16tochar(name->name, filename, 15);

    if (strstart(seek, filename) != 0)
        return 0;

    bool is_last = *advance_path(seek) == '\0';

    uint32_t filecluster = info->first_cluster;
    uint32_t bps = 1 << mbs->bytes_per_sector_shift;
    uint32_t spc = 1 << mbs->sectors_per_cluster_shift;
    uint32_t bpc = bps * spc;
    uint32_t count = (info->filesize + bpc - 1) / bpc;

    kprintf("Now handling %s. Last %i",(uintptr_t)seek, is_last);

    if (is_last)
        return ef_list_directory(count, filecluster);
    if (entry->flags.directory)
        return ef_walk_directory(count, filecluster, advance_path(seek), list_entries_handler);
    return 0;
}

string_list* ef_list_contents(const char *path){
    path = advance_path(path);

    return (string_list*)ef_walk_directory(1, mbs->first_cluster_of_root_directory, path, list_entries_handler);
}