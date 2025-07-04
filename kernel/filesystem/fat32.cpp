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

    uint32_t lba = cluster_start + ((root_index - 2) * cluster_size);

    kprintf("Reading cluster(s) %i-%i, starting from %i (LBA %i) Address %x", root_index, root_index+cluster_count, cluster_start, lba, lba * 512);

    void* buffer = (char*)allocate_in_page(fs_page, cluster_count * cluster_size * 512, ALIGN_64B, true, true);
    
    if (cluster_count > 0){
        uint32_t next_index = root_index;
        for (int i = 0; i < cluster_count; i++){
            kprintf("Cluster %i = %x (%x)",i,next_index,(cluster_start + ((next_index - 2) * cluster_size)) * 512);
            disk_read(buffer + (i * cluster_size * 512), cluster_start + ((next_index - 2) * cluster_size), cluster_size);
            next_index = fat[next_index];
            if (next_index >= 0x0FFFFFF8) return buffer;
        }
    }
    
    return buffer;
}

void FAT32FS::parse_longnames(f32longname entries[], uint16_t count, char* out){
    if (count == 0) return;
    uint16_t total = ((5+6+2)*count) + 1;
    uint16_t filename[total];
    memset(filename, 0, sizeof(filename));
    uint16_t f = 0;
    for (int i = count-1; i >= 0; i--){
        uint8_t* buffer = (uint8_t*)&entries[i];
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

void* FAT32FS::walk_directory(uint32_t cluster_count, uint32_t root_index, char *seek, f32_entry_handler handler) {
    uint32_t cluster_size = mbs->sectors_per_cluster;
    char *buffer = (char*)read_cluster(data_start_sector, cluster_size, cluster_count, root_index);
    f32file_entry *entry = 0;

    for (uint64_t i = 0; i < cluster_count * cluster_size * 512; i++) {
        bool long_name = buffer[i + 0xB] == 0xF;
        char filename[255];
        if (long_name){
            uint8_t order;
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
        i+= sizeof(f32file_entry) - 1;
        void *result = handler(this, entry, filename, seek);
        if (result)
            return result;
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

    for (uint64_t i = 0; i < cluster_count * cluster_size * 512; i++) {
        char c = buffer[i];
        count++;
        bool long_name = buffer[i + 0xB] == 0xF;
        char filename[255];
        if (long_name){
            uint8_t order;
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
    fat = (uint32_t*)allocate_in_page(fs_page, size * count * 512, ALIGN_64B, true, true);
    disk_read((void*)fat, location, size);
    total_fat_entries = (size * count * 512) / 4;
}

uint32_t FAT32FS::count_FAT(uint32_t first){
    uint32_t entry = fat[first];
    int count = 1;
    while (entry < 0x0FFFFFF8){
        entry = fat[entry];
        count++;
    }
    return count;
}

uint16_t read_unaligned16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

#define MBS_NUM_SECTORS read_unaligned16(mbs8 + 0x13)

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

    uint8_t *mbs8 = (uint8_t*)mbs;

    cluster_count = (MBS_NUM_SECTORS == 0 ? mbs->large_num_sectors : MBS_NUM_SECTORS)/mbs->sectors_per_cluster;
    data_start_sector = mbs->reserved_sectors + (mbs->sectors_per_fat * mbs->number_of_fats);

    if (mbs->first_cluster_of_root_directory > cluster_count){
        kprintf("[fat32 error] root directory cluster not found");
        return false;
    }

    bytes_per_sector = read_unaligned16(mbs8 + 0xB);

    kprintf("FAT32 Volume uses %i cluster size", bytes_per_sector);
    kprintf("Data start at %x",data_start_sector*512);

    read_FAT(mbs->reserved_sectors, mbs->sectors_per_fat, mbs->number_of_fats);

    return true;
}

void* FAT32FS::read_entry_handler(FAT32FS *instance, f32file_entry *entry, char *filename, char *seek) {
    if (entry->flags.volume_id) return 0;
    
    bool is_last = *instance->advance_path(seek) == '\0';
    if (!is_last && strstart(seek, filename, true) == 0) return 0;
    if (is_last && strcmp(seek, filename, true) != 0) return 0;

    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;
    uint32_t bps = instance->bytes_per_sector;
    uint32_t spc = instance->mbs->sectors_per_cluster;
    uint32_t bpc = bps * spc;
    uint32_t count = entry->filesize > 0 ? ((entry->filesize + bpc - 1) / bpc) : instance->count_FAT(filecluster);

    return entry->flags.directory
        ? instance->walk_directory(count, filecluster, instance->advance_path(seek), read_entry_handler)
        : instance->read_full_file(instance->data_start_sector, instance->mbs->sectors_per_cluster, count, entry->filesize, filecluster);
}

void* FAT32FS::read_file(char *path){
    path = advance_path(path);

    uint32_t count = count_FAT(mbs->first_cluster_of_root_directory);
    return walk_directory(count, mbs->first_cluster_of_root_directory, path, read_entry_handler);
}

void* FAT32FS::list_entries_handler(FAT32FS *instance, f32file_entry *entry, char *filename, char *seek) {

    if (entry->flags.volume_id) return 0;
    if (strstart(seek, filename, true) == 0) return 0;
    
    bool is_last = *instance->advance_path(seek) == '\0';
    
    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;

    uint32_t bps = instance->bytes_per_sector;
    uint32_t spc = instance->mbs->sectors_per_cluster;
    uint32_t bpc = bps * spc;
    uint32_t count = instance->count_FAT(filecluster);

    kprintf("Final count %i", count);

    if (is_last) return instance->list_directory(count, filecluster);
    if (entry->flags.directory) return instance->walk_directory(count, filecluster, instance->advance_path(seek), list_entries_handler);
    return 0;
}

string_list* FAT32FS::list_contents(char *path){
    path = advance_path(path);

    uint32_t count = count_FAT(mbs->first_cluster_of_root_directory);
    kprintf("Total clusters of root directory %i",count);
    return (string_list*)walk_directory(count, mbs->first_cluster_of_root_directory, path, list_entries_handler);
}