#include "exfat.hpp"
#include "disk.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "std/string.h"
#include "std/memfunctions.h"

char* ExFATFS::advance_path(char *path){
    while (*path != '/' && *path != '\0')
        path++;
    path++;
    return path;
}

void* ExFATFS::read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index){
    uint32_t count = cluster_count * cluster_size;

    uint32_t lba = cluster_start + ((root_index - 2) * cluster_size);

    kprintf("Reading cluster(s) %i-%i, starting from %i (LBA %i)", root_index, root_index+cluster_count, cluster_start, lba);

    void* buffer = (char*)allocate_in_page(fs_page, cluster_count * cluster_size * 512, ALIGN_64B, true, true);
    
    disk_read(buffer, lba, count);
    
    return buffer;
}

void* ExFATFS::walk_directory(uint32_t cluster_count, uint32_t root_index, char *seek, ef_entry_handler handler) {
    uint32_t cluster_size = 1 << mbs->sectors_per_cluster_shift;
    char *buffer = (char*)read_cluster(mbs->cluster_heap_offset, cluster_size, cluster_count, root_index);
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
                void *result = handler(this,entry, entry1, entry2, seek);
                if (result)
                    return result;
            }
            i += sizeof(filename_entry) - 1;
        }
    }

    return 0;
}

void* ExFATFS::list_directory(uint32_t cluster_count, uint32_t root_index) {
    uint32_t cluster_size = 1 << mbs->sectors_per_cluster_shift;
    char *buffer = (char*)read_cluster(mbs->cluster_heap_offset, cluster_size, cluster_count, root_index);
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

void* ExFATFS::read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index){

    char *buffer = (char*)read_cluster(cluster_start, cluster_size, cluster_count, root_index);

    void *file = allocate_in_page(fs_page, file_size, ALIGN_64B, true, true);

    memcpy(file, (void*)buffer, file_size);
    
    return file;
}

void ExFATFS::read_FAT(uint32_t location, uint32_t size, uint8_t count){
    uint32_t* fat = (uint32_t*)allocate_in_page(fs_page, size * count * 512, ALIGN_64B, true, true);
    disk_read((void*)fat, location, size);
    kprintf("FAT: %x (%x)",location*512,size * count * 512);
    uint32_t total_entries = (size * count * 512) / 4;
    // for (uint32_t i = 0; i < total_entries; i++)
    //     if (fat[i] != 0) kprintf("[%i] = %x", i, fat[i]);
}

bool ExFATFS::init(){
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
    read_FAT(mbs->fat_offset, mbs->fat_length, mbs->number_of_fats);
    return true;
}

void* ExFATFS::read_entry_handler(ExFATFS *instance, file_entry *entry, fileinfo_entry *info, filename_entry *name, char *seek) {
    char filename[15];
    utf16tochar(name->name, filename, 15);

    if (strstart(seek, filename) != 0)
        return 0;

    uint32_t filecluster = info->first_cluster;
    uint32_t bps = 1 << instance->mbs->bytes_per_sector_shift;
    uint32_t spc = 1 << instance->mbs->sectors_per_cluster_shift;
    uint32_t bpc = bps * spc;
    uint32_t count = (info->filesize + bpc - 1) / bpc;

    return entry->flags.directory
        ? instance->walk_directory(count, filecluster, instance->advance_path(seek), read_entry_handler)
        : instance->read_full_file(instance->mbs->cluster_heap_offset, 1 << instance->mbs->sectors_per_cluster_shift, count, info->filesize, filecluster);
}

void* ExFATFS::read_file(char *path){
    path = advance_path(path);

    return walk_directory(1, mbs->first_cluster_of_root_directory, path, read_entry_handler);
}

void* ExFATFS::list_entries_handler(ExFATFS *instance, file_entry *entry, fileinfo_entry *info, filename_entry *name, char *seek) {
    char filename[15];
    utf16tochar(name->name, filename, 15);

    if (strstart(seek, filename) != 0)
        return 0;

    bool is_last = *instance->advance_path(seek) == '\0';

    uint32_t filecluster = info->first_cluster;
    uint32_t bps = 1 << instance->mbs->bytes_per_sector_shift;
    uint32_t spc = 1 << instance->mbs->sectors_per_cluster_shift;
    uint32_t bpc = bps * spc;
    uint32_t count = (info->filesize + bpc - 1) / bpc;

    kprintf("Now handling %s. Last %i",(uintptr_t)seek, is_last);

    if (is_last)
        return instance->list_directory(count, filecluster);
    if (entry->flags.directory)
        return instance->walk_directory(count, filecluster, instance->advance_path(seek), list_entries_handler);
    return 0;
}

string_list* ExFATFS::list_contents(char *path){
    path = advance_path(path);

    return (string_list*)walk_directory(1, mbs->first_cluster_of_root_directory, path, list_entries_handler);
}