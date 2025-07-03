#include "fat32.hpp"
#include "disk.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "std/string.h"
#include "std/memfunctions.h"
#include "math/math.h"

char* FAT32FS::advance_path(char *path){
    while (*path != '/' && *path != '\0')
        path++;
    path++;
    return path;
}

void* FAT32FS::read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index){
    uint32_t count = cluster_count * cluster_size;

    uint32_t lba = cluster_start + ((root_index - 2) * cluster_size);

    kprintf("Reading cluster(s) %i-%i, starting from %i (LBA %i) Address %x", root_index, root_index+cluster_count, cluster_start, lba, lba * 512);

    void* buffer = (char*)allocate_in_page(fs_page, cluster_count * cluster_size * 512, ALIGN_64B, true, true);
    
    if (count > 0)
        disk_read(buffer, lba, count);
    
    return buffer;
}

void* FAT32FS::walk_directory(uint32_t cluster_count, uint32_t root_index, char *seek, f32_entry_handler handler) {
    uint32_t cluster_size = mbs->sectors_per_cluster;
    char *buffer = (char*)read_cluster(data_start_sector, cluster_size, cluster_count, root_index);
    f32file_entry *entry = 0;

    for (uint64_t i = 0; i < cluster_size * 512; i++) {
        char c = buffer[i];
        entry = (f32file_entry *)&buffer[i];
        void *result = handler(this, entry, seek);
        if (result)
            return result;
        i+= sizeof(f32file_entry) - 1;
    }

    return 0;
}

void* FAT32FS::list_directory(uint32_t cluster_count, uint32_t root_index) {
    uint32_t cluster_size = mbs->sectors_per_cluster;
    char *buffer = (char*)read_cluster(data_start_sector, cluster_size, cluster_count, root_index);
    f32file_entry *entry = 0;
    void *list_buffer = (char*)allocate_in_page(fs_page, 0x1000 * cluster_count, ALIGN_64B, true, true);
    uint32_t len = 0; 
    uint32_t count = 0;

    char *write_ptr = (char*)list_buffer + 4;

    for (uint64_t i = 0; i < cluster_size * 512; i++) {
        char c = buffer[i];
        entry = (f32file_entry *)&buffer[i];
        count++;

        bool ext_found = false;
        for (int j = 0; j < 11 && entry->filename[j]; j++){
            if (entry->filename[j] != ' ')
                *write_ptr++ = entry->filename[j];
            else if (!ext_found){
                *write_ptr++ = '.';
                ext_found = true;
            }
        }
        *write_ptr++ = '\0';
        i += sizeof(f32file_entry) - 1;
    }

    *(uint32_t*)list_buffer = count;

    return list_buffer;
}

void* FAT32FS::read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index){

    char *buffer = (char*)read_cluster(cluster_start, cluster_size, cluster_count, root_index);

    void *file = allocate_in_page(fs_page, file_size, ALIGN_64B, true, true);

    memcpy(file, (void*)buffer, file_size);
    
    return file;
}

void FAT32FS::read_FAT(uint32_t location, uint32_t size, uint8_t count){
    // kprintf("FAT: %x (%x)",location*512,size * count * 512);
    uint32_t* fat = (uint32_t*)allocate_in_page(fs_page, size * count * 512, ALIGN_64B, true, true);
    disk_read((void*)fat, location, size);
    uint32_t total_entries = (size * count * 512) / 4;
    // for (uint32_t i = 0; i < total_entries; i++)
    //     if (fat[i] != 0) kprintf("[%i] = %x", i, fat[i]);
}

bool FAT32FS::init(){
    fs_page = alloc_page(0x1000, true, true, false);

    mbs = (fat32_mbs*)allocate_in_page(fs_page, 512, ALIGN_64B, true, true);
    
    disk_read((void*)mbs, 0, 1);

    if (mbs->boot_signature != 0xAA55){
        kprintf("[fat32] Wrong boot signature %x",mbs->boot_signature);
        return false;
    }
    if (mbs->signature != 0x29 && mbs->signature != 0x28){
        kprintf("[fat32 error] Wrong signature %x",mbs->signature);
        return false;
    }

    cluster_count = (mbs->num_sectors == 0 ? mbs->large_num_sectors : mbs->num_sectors)/mbs->sectors_per_cluster;
    data_start_sector = mbs->reserved_sectors + (mbs->sectors_per_fat * mbs->number_of_fats);

    if (mbs->first_cluster_of_root_directory > cluster_count){
        kprintf("[fat32 error] root directory cluster not found");
        return false;
    }

    kprintf("FAT32 Volume uses %i cluster size", mbs->bytes_per_sector);
    kprintf("Data start at %x",data_start_sector*512);

    read_FAT(mbs->reserved_sectors, mbs->sectors_per_fat, mbs->number_of_fats);

    return true;
}

void* FAT32FS::read_entry_handler(FAT32FS *instance, f32file_entry *entry, char *seek) {
    char filename[11];
    if (entry->flags.volume_id) return 0;
    int j = 0;
    bool ext_found = false;
    for (int i = 0; i < 11 && entry->filename[i]; i++){
        if (entry->filename[i] != ' ')
            filename[j++] = entry->filename[i];
        else if (!ext_found){
            filename[j++] = '.';
            ext_found = true;
        }
    }
    filename[j++] = '\0';
    bool is_last = *instance->advance_path(seek) == '\0';
    if (!is_last && strstart(seek, filename, true) == 0) return 0;
    if (is_last && strcmp(seek, filename, true) != 0) return 0;

    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;
    uint32_t bps = instance->mbs->bytes_per_sector;
    uint32_t spc = instance->mbs->sectors_per_cluster;
    uint32_t bpc = bps * spc;
    uint32_t count = max(1,(entry->filesize + bpc - 1) / bpc);

    return entry->flags.directory
        ? instance->walk_directory(count, filecluster, instance->advance_path(seek), read_entry_handler)
        : instance->read_full_file(instance->data_start_sector, instance->mbs->sectors_per_cluster, count, entry->filesize, filecluster);
}

void* FAT32FS::read_file(char *path){
    path = advance_path(path);

    return walk_directory(1, mbs->first_cluster_of_root_directory, path, read_entry_handler);
}

void* FAT32FS::list_entries_handler(FAT32FS *instance, f32file_entry *entry, char *seek) {

    if (entry->flags.volume_id) return 0;
    if (strstart(seek, entry->filename, true) == 0) return 0;
    
    bool is_last = *instance->advance_path(seek) == '\0';
    
    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;

    uint32_t bps = instance->mbs->bytes_per_sector;
    uint32_t spc = instance->mbs->sectors_per_cluster;
    uint32_t bpc = bps * spc;
    uint32_t count = max(1,(entry->filesize + bpc - 1) / bpc);

    // kprintf("Now handling %s. Last %i",(uintptr_t)seek, is_last);

    if (is_last) return instance->list_directory(count, filecluster);
    if (entry->flags.directory) return instance->walk_directory(count, filecluster, instance->advance_path(seek), list_entries_handler);
    return 0;
}

string_list* FAT32FS::list_contents(char *path){
    path = advance_path(path);

    return (string_list*)walk_directory(1, mbs->first_cluster_of_root_directory, path, list_entries_handler);
}