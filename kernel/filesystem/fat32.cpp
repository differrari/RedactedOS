#include "fat32.hpp"
#include "disk.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "std/memory_access.h"
#include "std/string.h"
#include "std/memory.h"
#include "math/math.h"

#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

bool FAT32FS::init(uint32_t partition_sector){
    fs_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, false);

    mbs = (fat32_mbs*)kalloc(fs_page, 512, ALIGN_64B, MEM_PRIV_KERNEL);

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

    open_files = chashmap_create(128);

    return true;
}

sizedptr FAT32FS::read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index){

    uint32_t lba = cluster_start + ((root_index - 2) * cluster_size);

    kprintfv("Reading cluster(s) %i-%i, starting from %i (LBA %i) Address %x", root_index, root_index+cluster_count, cluster_start, lba, lba * 512);

    size_t size = cluster_count * cluster_size * 512;
    void* buffer = kalloc(fs_page, size, ALIGN_64B, MEM_PRIV_KERNEL);
    
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
    uint16_t f = 0;
    for (int i = count-1; i >= 0; i--){
        uint8_t *buffer = (uint8_t*)&entries[i];
        for (int j = 0; j < 5; j++){
            out[f++] = buffer[1+(j*2)];
        }
        for (int j = 0; j < 6; j++){
            out[f++] = buffer[14+(j*2)];
        }
        for (int j = 0; j < 2; j++){
            out[f++] = buffer[28+(j*2)];
        }
    }
    out[f++] = '\0';
}

void FAT32FS::parse_shortnames(f32file_entry* entry, char* out){
    int j = 0;
    bool ext_found = false;
    for (int i = 0; i < 11 && entry->filename[i]; i++){
        if (entry->filename[i] != ' '){
            out[j++] = entry->filename[i];
            if (i == 7 && !ext_found){
                out[j++] = '.';
                ext_found = true;
            }
        }
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
        char filename[256];
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
        if (!long_name)
            parse_shortnames(entry, filename);
        sizedptr result = handler(this, entry, filename, seek);
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
    size_t full_size = 0x1000 * cluster_count;
    void *list_buffer = (char*)kalloc(fs_page, full_size, ALIGN_64B, MEM_PRIV_KERNEL);
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
        char filename[256];
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
        if (!long_name)
            parse_shortnames(entry, filename);
        char *f = filename;
        while (*f) {
            *write_ptr++ = *f;
            f++;
        }
        *write_ptr++ = '\0';
        i += sizeof(f32file_entry);
    }

    *(uint32_t*)list_buffer = count;

    return (sizedptr){(uintptr_t)list_buffer, full_size};
}

sizedptr FAT32FS::read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index){

    sizedptr buf_ptr = read_cluster(cluster_start, cluster_size, cluster_count, root_index);
    char *buffer = (char*)buf_ptr.ptr;

    void *file = kalloc(fs_page, file_size, ALIGN_64B, MEM_PRIV_KERNEL);

    memcpy(file, (void*)buffer, file_size);
    
    return (sizedptr){(uintptr_t)file,file_size};
}

void FAT32FS::read_FAT(uint32_t location, uint32_t size, uint8_t count){
    fat = (uint32_t*)kalloc(fs_page, size * count * 512, ALIGN_64B, MEM_PRIV_KERNEL);
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
    
    bool is_last = *seek_to(seek, '/') == '\0';
    if (!is_last && strstart_case(seek, filename,true) < (int)(strlen_max(filename, 0)-1)) return {0, 0};
    if (is_last && strcmp_case(seek, filename,true) != 0) return {0, 0};

    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;
    uint32_t bps = instance->bytes_per_sector;
    uint32_t spc = instance->mbs->sectors_per_cluster;
    uint32_t bpc = bps * spc;
    uint32_t count = entry->filesize > 0 ? ((entry->filesize + bpc - 1) / bpc) : instance->count_FAT(filecluster);

    return entry->flags.directory
        ? instance->walk_directory(count, filecluster, seek_to(seek, '/'), read_entry_handler)
        : instance->read_full_file(instance->data_start_sector, instance->mbs->sectors_per_cluster, count, entry->filesize, filecluster);
}

FS_RESULT FAT32FS::open_file(const char* path, file* descriptor){
    if (!mbs) return FS_RESULT_DRIVER_ERROR;
    uint64_t fid = reserve_fd_gid(path);
    module_file *mfile = (module_file*)chashmap_get(open_files, &fid, sizeof(uint64_t));
    if (mfile){
        descriptor->id = mfile->fid;
        descriptor->size = mfile->file_size;
        mfile->references++;
        return FS_RESULT_SUCCESS;
    }
    path = seek_to(path, '/');
    uint32_t count = count_FAT(mbs->first_cluster_of_root_directory);
    sizedptr buf_ptr = walk_directory(count, mbs->first_cluster_of_root_directory, path, read_entry_handler);
    void *buf = (void*)buf_ptr.ptr;
    if (!buf || !buf_ptr.size) return FS_RESULT_NOTFOUND;
    descriptor->id = fid;
    descriptor->size = buf_ptr.size;
    mfile = (module_file*)kalloc(fs_page, sizeof(module_file), ALIGN_64B, MEM_PRIV_KERNEL);
    mfile->file_size = buf_ptr.size;
    mfile->buffer = (uintptr_t)buf;
    mfile->ignore_cursor = false;
    mfile->fid = descriptor->id;
    mfile->references = 1;
    return chashmap_put(open_files, &fid, sizeof(uint64_t), mfile) >= 0 ? FS_RESULT_SUCCESS : FS_RESULT_DRIVER_ERROR;
}

size_t FAT32FS::read_file(file *descriptor, void* buf, size_t size){
    module_file *mfile  = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return 0;
    if (descriptor->cursor > mfile->file_size) return 0;
    if (size > mfile->file_size-descriptor->cursor) size = mfile->file_size-descriptor->cursor;
    memcpy(buf, (void*)(mfile->buffer + descriptor->cursor), size);
    return size;
}

void FAT32FS::close_file(file* descriptor){
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return;
    mfile->references--;
    if (mfile->references == 0){
        chashmap_remove(open_files, &descriptor->id, sizeof(uint64_t), 0);
        kfree((void*)mfile->buffer, mfile->file_size);
    }
}

sizedptr FAT32FS::list_entries_handler(FAT32FS *instance, f32file_entry *entry, char *filename, const char *seek) {

    if (entry->flags.volume_id) return { 0, 0 };
    if (strstart_case(seek, filename,true) != (int)(strlen_max(filename, 0)-1)) return { 0, 0 };
    
    bool is_last = *seek_to(seek, '/') == '\0';
    
    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;

    uint32_t count = instance->count_FAT(filecluster);

    if (is_last) return instance->list_directory(count, filecluster);
    if (entry->flags.directory) return instance->walk_directory(count, filecluster, seek_to(seek, '/'), list_entries_handler);
    return { 0, 0 };
}

size_t FAT32FS::list_contents(const char *path, void* buf, size_t size, uint64_t *offset){
    if (!mbs) return 0;
    path = seek_to(path, '/');

    uint32_t count_sectors = count_FAT(mbs->first_cluster_of_root_directory);
    sizedptr ptr = walk_directory(count_sectors, mbs->first_cluster_of_root_directory, path, list_entries_handler);
    
    size = min(size, ptr.size);

    uint32_t count = 0;
    uint32_t total_count = *(uint32_t*)ptr.ptr;
	
    char *write_ptr = (char*)buf + 4;
    char *cursor = (char*)ptr.ptr + 4;

    bool offset_found = !offset || *offset == 0 ? true : false;

    for (uint32_t i = 0; i < total_count; i++){
    	size_t len = strlen(cursor);
    	uint64_t hash = chashmap_fnv1a64(cursor, len);
    	if (!offset_found){
		if (hash == *offset) offset_found = true;
		else kprintf("File hash %llx for %s",hash,cursor);
		cursor += len + 1;
    		continue;
    	}
    	if ((uintptr_t)write_ptr + len < (uintptr_t)buf + size){
    		memcpy(write_ptr, cursor, len);
    		write_ptr += len;
    		*write_ptr++ = 0;
    		cursor += len + 1;
    		count++;
    	} else {
    		*offset = hash;
    		break;
    	}
    }

    *(uint32_t*)buf = count;

    return (uintptr_t)write_ptr-(uintptr_t)buf;
}
