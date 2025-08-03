#include "fat32.hpp"
#include "disk.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "memory/memory_access.h"
#include "std/string.h"
#include "std/memfunctions.h"
#include "math/math.h"

#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

const char* FAT32FS::advance_path(const char *path){
    while (*path != '/' && *path != '\0')
        path++;
    path++;
    return path;
}

bool FAT32FS::init(uint32_t partition_sector){
    fs_page = palloc(0x1000, true, true, false);

    mbs = (fat32_mbs*)kalloc(fs_page, 512, ALIGN_64B, true, true);

    partition_first_sector = partition_sector;
    
    disk_read((void*)mbs, partition_first_sector, 1);

    kprintf("[fat32] Reading fat32 mbs at %x. %x",partition_first_sector, mbs->jumpboot[0]);

    if (mbs->boot_signature != 0xAA55){
        kprintf("[fat32] Wrong boot signature %x",mbs->boot_signature);
        return false;
    }
    if (mbs->signature != 0x29 && mbs->signature != 0x28){
        kprintf("[fat32 error] Wrong signature %x",mbs->signature);
        return false;
    }

    uint16_t num_sectors = read_unaligned16(&mbs->num_sectors);

    cluster_count = (num_sectors == 0 ? mbs->large_num_sectors : num_sectors)/mbs->sectors_per_cluster;
    data_start_sector = mbs->reserved_sectors + (mbs->sectors_per_fat * mbs->number_of_fats);

    if (mbs->first_cluster_of_root_directory > cluster_count){
        kprintf("[fat32 error] root directory cluster not found");
        return false;
    }

    bytes_per_sector = read_unaligned16(&mbs->bytes_per_sector);

    kprintf("FAT32 Volume uses %i cluster size", bytes_per_sector);
    kprintf("Data start at %x",data_start_sector*512);
    read_FAT(mbs->reserved_sectors, mbs->sectors_per_fat, mbs->number_of_fats);

    open_files = IndexMap<void*>(128);

    return true;
}

sizedptr FAT32FS::read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index){

    uint32_t lba = cluster_start + ((root_index - 2) * cluster_size);

    kprintfv("Reading cluster(s) %i-%i, starting from %i (LBA %i) Address %x", root_index, root_index+cluster_count, cluster_start, lba, lba * 512);

    size_t size = cluster_count * cluster_size * 512;
    void* buffer = kalloc(fs_page, size, ALIGN_64B, true, true);
    
    if (cluster_count > 0){
        uint32_t next_index = root_index;
        for (uint32_t i = 0; i < cluster_count; i++){
            kprintfv("Cluster %i = %x (%x)",i,next_index,(cluster_start + ((next_index - 2) * cluster_size)) * 512);
            disk_read((void*)((uintptr_t)buffer + (i * cluster_size * 512)), partition_first_sector + cluster_start + ((next_index - 2) * cluster_size), cluster_size);
            next_index = fat[next_index];
            if (next_index >= 0x0FFFFFF8) return (sizedptr){ (uintptr_t)buffer, size };
        }
    }
    
    return (sizedptr){ (uintptr_t)buffer, size };
}

void FAT32FS::parse_longnames(f32longname entries[], uint16_t count, char* out){
    if (count == 0) return;
    uint16_t total = ((5+6+2)*count) + 1;
    uint16_t *filename = (uint16_t*)kalloc(fs_page, total*2, ALIGN_64B, true, true);
    uint16_t f = 0;
    for (int i = count-1; i >= 0; i--){
        uint8_t *buffer = (uint8_t*)&entries[i];
        for (int j = 0; j < 5; j++){
            filename[f++] = (buffer[1+(j*2)] << 8) | buffer[1+(j*2) + 1];
        }
        for (int j = 0; j < 6; j++){
            filename[f++] = (buffer[14+(j*2)] << 8) | buffer[1+(j*2) + 1];
        }
        for (int j = 0; j < 2; j++){
            filename[f++] = (buffer[18+(j*2)] << 8) | buffer[1+(j*2) + 1];
        }
    }
    filename[f++] = '\0';
    utf16tochar(filename, out, f);
    kfree(filename, total*2);
}

void FAT32FS::parse_shortnames(f32file_entry* entry, char* out){
    int j = 0;
    bool ext_found = false;
    for (int i = 0; i < 11 && entry->filename[i]; i++){
        if (entry->filename[i] != ' ')
            out[j++] = entry->filename[i];
        else if (!ext_found){
            out[j++] = '.';
            ext_found = true;
        }
    }
    out[j++] = '\0';
}

sizedptr FAT32FS::walk_directory(uint32_t cluster_count, uint32_t root_index, const char *seek, f32_entry_handler handler) {
    uint32_t cluster_size = mbs->sectors_per_cluster;
    sizedptr buf_ptr = read_cluster(data_start_sector, cluster_size, cluster_count, root_index);
    char *buffer = (char*)buf_ptr.ptr;
    f32file_entry *entry = 0;

    for (uint64_t i = 0; i < cluster_count * cluster_size * 512;) {
        if (buffer[i] == 0) return {0 , 0};
        if (buffer[i] == 0xE5){
            i += sizeof(f32file_entry);
            continue;
        }
        bool long_name = buffer[i + 0xB] == 0xF;
        char *filename = (char*)kalloc(fs_page, 255, ALIGN_64B, true, true);
        if (long_name){
            f32longname *first_longname = (f32longname*)&buffer[i];
            uint16_t count = 0;
            do {
                i += sizeof(f32longname);
                count++;
            } while (buffer[i + 0xB] == 0xF);
            parse_longnames(first_longname, count, filename);
        }
        entry = (f32file_entry *)&buffer[i];
        if (!long_name){
            parse_shortnames(entry, filename);
        }
        sizedptr result = handler(this, entry, filename, seek);
        kfree(filename, 255);
        if (result.ptr && result.size)
            return result;
        i += sizeof(f32file_entry);
    }

    return { 0,0 };
}

sizedptr FAT32FS::list_directory(uint32_t cluster_count, uint32_t root_index) {
    if (!mbs) return { 0, 0};
    uint32_t cluster_size = mbs->sectors_per_cluster;
    sizedptr buf_ptr = read_cluster(data_start_sector, cluster_size, cluster_count, root_index);
    char *buffer = (char*)buf_ptr.ptr;
    f32file_entry *entry = 0;
    void *list_buffer = (char*)kalloc(fs_page, 0x1000 * cluster_count, ALIGN_64B, true, true);
    uint32_t count = 0;

    char *write_ptr = (char*)list_buffer + 4;

    for (uint64_t i = 0; i < cluster_count * cluster_size * 512;) {
        if (buffer[i] == 0) break;
        if (buffer[i] == 0xE5){
            i += sizeof(f32file_entry);
            continue;
        }
        count++;
        bool long_name = buffer[i + 0xB] == 0xF;
        char *filename = (char*)kalloc(fs_page, 255, ALIGN_64B, true, true);
        if (long_name){
            f32longname *first_longname = (f32longname*)&buffer[i];
            uint16_t count = 0;
            do {
                i += sizeof(f32longname);
                count++;
            } while (buffer[i + 0xB] == 0xF);
            parse_longnames(first_longname, count, filename);
        }
        entry = (f32file_entry *)&buffer[i];
        if (!long_name){
            parse_shortnames(entry, filename);
        }
        char *f = filename;
        while (*f) {
            *write_ptr++ = *f;
            f++;
        }
        *write_ptr++ = '\0';
        kfree(filename, 255);
        i += sizeof(f32file_entry);
    }

    *(uint32_t*)list_buffer = count;

    return (sizedptr){(uintptr_t)list_buffer, (uintptr_t)write_ptr-(uintptr_t)list_buffer};
}

sizedptr FAT32FS::read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index){

    sizedptr buf_ptr = read_cluster(cluster_start, cluster_size, cluster_count, root_index);
    char *buffer = (char*)buf_ptr.ptr;

    void *file = kalloc(fs_page, file_size, ALIGN_64B, true, true);

    memcpy(file, (void*)buffer, file_size);
    
    return (sizedptr){(uintptr_t)file,file_size};
}

void FAT32FS::read_FAT(uint32_t location, uint32_t size, uint8_t count){
    fat = (uint32_t*)kalloc(fs_page, size * count * 512, ALIGN_64B, true, true);
    disk_read((void*)fat, partition_first_sector + location, size);
    total_fat_entries = (size * count * 512) / 4;
}

uint32_t FAT32FS::count_FAT(uint32_t first){
    uint32_t entry = fat[first];
    int count = 1;
    while (entry < 0x0FFFFFF8 && entry != 0){
        entry = fat[entry];
        count++;
    }
    return count;
}

sizedptr FAT32FS::read_entry_handler(FAT32FS *instance, f32file_entry *entry, char *filename, const char *seek) {
    if (entry->flags.volume_id) return {0,0};
    
    bool is_last = *instance->advance_path(seek) == '\0';
    if (!is_last && strstart(seek, filename, true) == 0) return {0, 0};
    if (is_last && strcmp(seek, filename, true) != 0) return {0, 0};

    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;
    uint32_t bps = instance->bytes_per_sector;
    uint32_t spc = instance->mbs->sectors_per_cluster;
    uint32_t bpc = bps * spc;
    uint32_t count = entry->filesize > 0 ? ((entry->filesize + bpc - 1) / bpc) : instance->count_FAT(filecluster);

    return entry->flags.directory
        ? instance->walk_directory(count, filecluster, instance->advance_path(seek), read_entry_handler)
        : instance->read_full_file(instance->data_start_sector, instance->mbs->sectors_per_cluster, count, entry->filesize, filecluster);
}

FS_RESULT FAT32FS::open_file(const char* path, file* descriptor){
    if (!mbs) return FS_RESULT_DRIVER_ERROR;
    path = advance_path(path);
    uint32_t count = count_FAT(mbs->first_cluster_of_root_directory);
    sizedptr buf_ptr = walk_directory(count, mbs->first_cluster_of_root_directory, path, read_entry_handler);
    void *buf = (void*)buf_ptr.ptr;
    if (!buf) return FS_RESULT_NOTFOUND;
    descriptor->id = reserve_fd_id();
    descriptor->size = buf_ptr.size;
    open_files.add(reserve_fd_id(), buf);
    //TODO: go back to using a linked list, and a static id for the file, ideally global for system
    return FS_RESULT_SUCCESS;
}

size_t FAT32FS::read_file(file *descriptor, void* buf, size_t size){
    void* file = open_files[descriptor->id];
    //TODO: keep track of file size and limit copy to only that or less
    memcpy(buf, file, size);
    return size;
}

sizedptr FAT32FS::list_entries_handler(FAT32FS *instance, f32file_entry *entry, char *filename, const char *seek) {

    if (entry->flags.volume_id) return { 0, 0 };
    if (strstart(seek, filename, true) == 0) return { 0, 0 };
    
    bool is_last = *instance->advance_path(seek) == '\0';
    
    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;

    uint32_t count = instance->count_FAT(filecluster);

    if (is_last) return instance->list_directory(count, filecluster);
    if (entry->flags.directory) return instance->walk_directory(count, filecluster, instance->advance_path(seek), list_entries_handler);
    return { 0, 0 };
}

sizedptr FAT32FS::list_contents(const char *path){
    if (!mbs) return { 0, 0 };
    path = advance_path(path);

    uint32_t count = count_FAT(mbs->first_cluster_of_root_directory);
    return walk_directory(count, mbs->first_cluster_of_root_directory, path, list_entries_handler);
}
