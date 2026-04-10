#include "fat32.hpp"
#include "disk.h"
#include "memory/page_allocator.h"
#include "console/kio.h"
#include "std/memory_access.h"
#include "std/string.h"
#include "std/memory.h"
#include "math/math.h"
#include "syscalls/syscalls.h"
#include "exceptions/irq.h"

#define kprintfv(fmt, ...) \
    ({ \
        if (verbose){\
            kprintf(fmt, ##__VA_ARGS__); \
        }\
    })

bool FAT32FS::init(uint32_t partition_sector){
    fs_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_DEV | MEM_RW, false);

    mbs = (fat32_mbs*)kalloc(fs_page, 512, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!mbs) return false;

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

    open_files = chashmap_create(512);

    return fat && open_files;
}

sizedptr FAT32FS::read_cluster(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint32_t root_index){
    if (!cluster_count || !cluster_size) return (sizedptr){0, 0};
    if (root_index < 2 || root_index >= total_fat_entries) return (sizedptr){ 0, 0};

    size_t size = cluster_count * cluster_size * 512;
    void* buffer = kalloc(fs_page, size, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!buffer) return (sizedptr){0, 0};
    
    uint32_t next_index = root_index;
    for (uint32_t i = 0; i < cluster_count; i++){
        if (next_index < 2 || next_index >= total_fat_entries){
            kprintfv("Cluster %i = %x (%x)",i,next_index,(cluster_start + ((next_index - 2) * cluster_size)) * 512);
            return (sizedptr){(uintptr_t)buffer, i * cluster_size * 512};
        }

        uint32_t current_lba = partition_first_sector + data_start_sector + ((next_index - 2) * cluster_size);
        kprintfv("cluster %i = %x (%x)", i, next_index, current_lba * 512);
        disk_read((void*)((uintptr_t)buffer + (i * cluster_size * 512)), current_lba, cluster_size);
        next_index = fat[next_index] & 0x0FFFFFFF;
        if (next_index >= 0x0FFFFFF8) return (sizedptr){ (uintptr_t)buffer, size };
    }
    
    return (sizedptr){ (uintptr_t)buffer, size };
}

bool FAT32FS::write_to_disk(u32 root_index, void *buf, size_t size){
    u32 cluster_size = mbs->sectors_per_cluster;

    u32 new_cluster_count = ceil((float)size/(cluster_size * 512));
    u32 old_cluster_count = count_FAT(root_index);
    
    if (new_cluster_count > old_cluster_count){
        //Allocate new cluster(s)
    } else if (new_cluster_count < old_cluster_count){
        //Truncate number of clusters
    }
    
    uint32_t next_index = root_index;
    for (uint32_t i = 0; i < new_cluster_count; i++){
        int32_t current_lba = partition_first_sector + data_start_sector + ((next_index - 2) * cluster_size);
        disk_write((void*)((uptr)buf + (i * cluster_size * 512)), current_lba, cluster_size);
        next_index = fat[next_index] & 0x0FFFFFFF;
        if (next_index >= 0x0FFFFFF8) return true;
    }
    
    return true;
}

void FAT32FS::parse_longnames(f32longname entries[], uint16_t count, char* out){
    if (count == 0 || !out) return;
    uint16_t f = 0;
    for (int i = count-1; i >= 0; i--){
        uint8_t *buffer = (uint8_t*)&entries[i];
        for (int j = 0; j < 5; j++){
            char c = (char)buffer[1 + (j * 2)];
            if (!c || c == (char)0xFF) out[f] = '\0';
            out[f++] = c;
        }
        for (int j = 0; j < 6; j++){
            char c = (char)buffer[14 + (j * 2)];
            if (!c || c == (char)0xFF) out[f] = '\0';
            out[f++] = c;
        }
        for (int j = 0; j < 2; j++){
            char c = (char)buffer[28 + (j * 2)];
            if (!c || c == (char)0xFF) out[f] = '\0';
            out[f++] = c;
        }
    }
}

void FAT32FS::parse_shortnames(f32file_entry* entry, char* out){
    if (!entry || !out) return;
    int j = 0;
    int base_end = 8;
    int ext_end = 11;
    while (base_end > 0 && entry->filename[base_end - 1] == ' ') base_end--;
    while (ext_end > 8 && entry->filename[ext_end - 1] == ' ') ext_end--;
    for (int i = 0; i < base_end; i++) out[j++] = entry->filename[i];
    if (ext_end > 8) {
        out[j++] = '.';
        for (int i = 8; i < ext_end; i++) out[j++] = entry->filename[i];
    }
    out[j] = '\0';
}

f32file_entry* FAT32FS::walk_directory(uint32_t cluster_count, uint32_t root_index, const char *seek, f32_entry_handler handler) {
    if (!mbs || !handler) return 0;
    uint32_t cluster_size = mbs->sectors_per_cluster;
    sizedptr buf_ptr = read_cluster(data_start_sector, cluster_size, cluster_count, root_index);
    char *buffer = (char*)buf_ptr.ptr;
    f32file_entry *entry = 0;

    if (!buffer || !buf_ptr.size) return 0;
    for (uint64_t i = 0; i + sizeof(f32file_entry) <= buf_ptr.size;) {
        if (buffer[i] == 0) {
            kfree(buffer, buf_ptr.size);
            return 0;
        }
        if (buffer[i] == 0xE5){
            i += sizeof(f32file_entry);
            continue;
        }
        bool long_name = buffer[i + 0xB] == 0xF;
        char filename[256] = {0};
        if (long_name){
            f32longname *first_longname = (f32longname*)&buffer[i];
            uint16_t count = 0;
            do {
                i += sizeof(f32longname);
                count++;
                if (i + sizeof(f32file_entry) > buf_ptr.size) {
                    kfree(buffer, buf_ptr.size);
                    return 0;
                }
            } while (buffer[i + 0xB] == 0xF);
            parse_longnames(first_longname, count, filename);
        } 
        entry = (f32file_entry *)&buffer[i];
        if (!long_name)
            parse_shortnames(entry, filename);
        kprintfv("[fat32] found entry: %s", filename);
        f32file_entry *found_entry = handler(this, entry, filename, seek);//TODO: should be possible to remove handlers altogether, moving their logic here
        if (found_entry) {
            if (found_entry == entry){
                f32file_entry *result = (f32file_entry*)zalloc(sizeof(f32file_entry));
                memcpy(result, found_entry, sizeof(f32file_entry));
                found_entry = result;
            }
            kfree(buffer, buf_ptr.size);
            return found_entry;
        }
        i += sizeof(f32file_entry);
    }

    kfree(buffer, buf_ptr.size);
    return 0;
}

sizedptr FAT32FS::list_directory(uint32_t cluster_count, uint32_t root_index) {
    if (!mbs) return { 0, 0};
    uint32_t cluster_size = mbs->sectors_per_cluster;
    sizedptr buf_ptr = read_cluster(data_start_sector, cluster_size, cluster_count, root_index);
    char *buffer = (char*)buf_ptr.ptr;
    f32file_entry *entry = 0;
    if (!buffer || !buf_ptr.size) return { 0, 0};
    size_t full_size = buf_ptr.size + 4;
    void *list_buffer = kalloc(fs_page, full_size, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!list_buffer) {
        kfree(buffer, buf_ptr.size);
        return { 0, 0};
    }

    uint32_t count = 0;

    char *write_ptr = (char*)list_buffer + 4;

    for (uint64_t i = 0; i + sizeof(f32file_entry) <= buf_ptr.size;) {
        if (buffer[i] == 0) break;
        if (buffer[i] == 0xE5){
            i += sizeof(f32file_entry);
            continue;
        }
        bool long_name = buffer[i + 0xB] == 0xF;
        char filename[256] = {0};
        if (long_name){
            f32longname *first_longname = (f32longname*)&buffer[i];
            uint16_t long_count = 0;
            do {
                i += sizeof(f32longname);
                long_count++;
                if (i + sizeof(f32file_entry) > buf_ptr.size) {
                    kfree(buffer, buf_ptr.size);
                    kfree(list_buffer, full_size);
                    return { 0, 0 };
                }
            } while (buffer[i + 0xB] == 0xF);
            parse_longnames(first_longname, long_count, filename);
        }
        entry = (f32file_entry *)&buffer[i];
        if (!long_name)
            parse_shortnames(entry, filename);
        if (entry->flags.volume_id) {
            i += sizeof(f32file_entry);
            continue;
        }
        count++;
        char *f = filename;
        while (*f) {
            *write_ptr++ = *f;
            f++;
        }
        *write_ptr++ = '\0';
        i += sizeof(f32file_entry);
    }

    *(uint32_t*)list_buffer = count;
    kfree(buffer, buf_ptr.size);

    return (sizedptr){(uintptr_t)list_buffer, full_size};
}

void* file_page;

sizedptr FAT32FS::read_full_file(uint32_t cluster_start, uint32_t cluster_size, uint32_t cluster_count, uint64_t file_size, uint32_t root_index){

    sizedptr buf_ptr = read_cluster(cluster_start, cluster_size, cluster_count, root_index);
    char *buffer = (char*)buf_ptr.ptr;
    if (!buffer || !buf_ptr.size) return {0, 0};

    if (!file_page) file_page = page_alloc(PAGE_SIZE);
    
    void *file = allocate(file_page, file_size ? file_size : 1, page_alloc);
    if (!file) {
        kfree(buffer, buf_ptr.size);
        return {0, 0};
    }

    if (file_size) memcpy(file, (void*)buffer, file_size);
    kfree(buffer, buf_ptr.size);
    
    return (sizedptr){(uintptr_t)file,file_size};
}

void FAT32FS::read_FAT(uint32_t location, uint32_t size, uint8_t count){
    fat = (uint32_t*)kalloc(fs_page, size * 512, ALIGN_64B, MEM_PRIV_KERNEL);
    if (!fat) {
        total_fat_entries = 0;
        return;
    }
    disk_read((void*)fat, partition_first_sector + location, size);
    total_fat_entries = (size * 512) / 4;
}

uint32_t FAT32FS::count_FAT(uint32_t first){
    if (!fat || first < 2 || first >= total_fat_entries) return 0;
    uint32_t entry = fat[first] & 0x0FFFFFFF;
    uint32_t count = 1;
    while (entry < 0x0FFFFFF8 && entry != 0){
        if (entry < 2 || entry >= total_fat_entries) {
            kprintf("[fat32] invalid FAT pointer %u", entry);
            return count;
        }
        entry = fat[entry] & 0x0FFFFFFF;
        count++;
    }
    return count;
}

f32file_entry* FAT32FS::read_entry_handler(FAT32FS *instance, f32file_entry *entry, char *filename, const char *seek) {
    if (entry->flags.volume_id) return 0;
    
    size_t name_len = strlen_max(filename, 0);
    int matched = strstart_case(seek, filename, true);
    if (matched != (int)name_len) return 0;

    const char *next = seek + name_len;
    if (*next == '/') next++;
    else if (*next != '\0') return 0;

    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;
    uint32_t bps = instance->bytes_per_sector;
    uint32_t spc = instance->mbs->sectors_per_cluster;
    uint32_t bpc = bps * spc;
    uint32_t count = entry->filesize > 0 ? ((entry->filesize + bpc - 1) / bpc) : instance->count_FAT(filecluster);
    
    if (entry->flags.directory) return instance->walk_directory(count, filecluster, next, read_entry_handler);
    if (*next != '\0') return 0;
    return entry;
}

FS_RESULT FAT32FS::open_file(const char* path, file* descriptor){
    if (!mbs) return FS_RESULT_DRIVER_ERROR;
    uint64_t fid = reserve_fd_gid(path);
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(open_files, &fid, sizeof(uint64_t));
    if (mfile){
        descriptor->id = mfile->fid;
        descriptor->size = mfile->file_size;
        mfile->references++;
        irq_restore(irq);
        return FS_RESULT_SUCCESS;
    }
    irq_restore(irq);
    const char *fullpath = path;
    path = seek_to(path, '/');
    uint32_t count = count_FAT(mbs->first_cluster_of_root_directory);
    f32file_entry *entry = walk_directory(count, mbs->first_cluster_of_root_directory, path, read_entry_handler);
    if (!entry) return FS_RESULT_NOTFOUND; 
    uint32_t filecluster = (entry->hi_first_cluster << 16) | entry->lo_first_cluster;
    uint32_t fat_count = count_FAT(filecluster);
    sizedptr buf_ptr = read_full_file(data_start_sector, mbs->sectors_per_cluster, fat_count, entry->filesize, filecluster);
    void *buf = (void*)buf_ptr.ptr;
    if (!buf || !buf_ptr.size) return FS_RESULT_NOTFOUND;
    descriptor->id = fid;
    descriptor->size = buf_ptr.size;
    mfile = (module_file*)kalloc(fs_page, sizeof(module_file), ALIGN_64B, MEM_PRIV_KERNEL);
    if (!mfile) {
        kfree(buf, buf_ptr.size ? buf_ptr.size : 1);
        return FS_RESULT_DRIVER_ERROR;
    }
    memset(mfile, 0, sizeof(module_file));
    mfile->file_size = buf_ptr.size;
    mfile->name = fullpath;
    mfile->file_buffer = (buffer){
        .buffer = buf,
        .buffer_size = buf_ptr.size,
        .limit = buf_ptr.size,
        .options = buffer_can_grow,
        .cursor = 0,
    };
    mfile->ignore_cursor = false;
    mfile->fid = descriptor->id;
    mfile->serial = filecluster;
    int32_t current_lba = partition_first_sector + data_start_sector + ((filecluster - 2) * mbs->sectors_per_cluster);
    mfile->references = 1;
    irq = irq_save_disable();
    int ok = chashmap_put(open_files, &fid, sizeof(uint64_t), mfile);
    irq_restore(irq);
    if (ok < 0) {
        kfree(mfile->file_buffer.buffer, mfile->file_size ? mfile->file_size : 1);
        kfree(mfile, sizeof(module_file));
        return FS_RESULT_DRIVER_ERROR;
    }
    return FS_RESULT_SUCCESS;
}

size_t FAT32FS::read_file(file *descriptor, void* buf, size_t size){
    irq_flags_t irq = irq_save_disable();
    module_file *mfile  = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) {
        irq_restore(irq);
        return 0;
    }
    if (descriptor->cursor > mfile->file_size) {
        irq_restore(irq);
        return 0;
    }
    if (size > mfile->file_size-descriptor->cursor) size = mfile->file_size-descriptor->cursor;
    memcpy(buf, (char*)mfile->file_buffer.buffer + descriptor->cursor, size);
    irq_restore(irq);
    return size;
}

size_t FAT32FS::write_file(file *descriptor, const char* buf, size_t size){
    kprintf("Writing file");
    module_file *mfile  = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) return 0;
    if (mfile->read_only) return 0;
    
    size_t written = buffer_write_to(&mfile->file_buffer, buf, size, descriptor->cursor);
    
    if (written)
        write_to_disk(mfile->serial, mfile->file_buffer.buffer, mfile->file_buffer.buffer_size);
    
    return written;
}

void FAT32FS::close_file(file* descriptor){
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    if (!mfile) {
        irq_restore(irq);
        return;
    }
    if (mfile->references) mfile->references--;
    if (mfile->references == 0){
        chashmap_remove(open_files, &descriptor->id, sizeof(uint64_t), 0);
        irq_restore(irq);
        buffer_destroy(&mfile->file_buffer);
        kfree(mfile, sizeof(module_file));
        return;
    }
    irq_restore(irq);
}

f32file_entry* FAT32FS::list_entries_handler(FAT32FS *instance, f32file_entry *entry, char *filename, const char *seek) {

    if (entry->flags.volume_id) return 0;
    size_t name_len = strlen_max(filename, 0);
    int matched = strstart_case(seek, filename,true);
    if (matched != (int)name_len) return 0;
    
    const char *next = seek + name_len;
    if (*next == '/') next++;
    else if (*next != '\0') return 0;
    
    if (!entry->flags.directory) return 0;

    uint32_t filecluster = (read_unaligned16(&entry->hi_first_cluster) << 16) | read_unaligned16(&entry->lo_first_cluster);
    uint32_t count = instance->count_FAT(filecluster);

    if (*next == '\0') return entry;
    return instance->walk_directory(count, filecluster, next, list_entries_handler);
}

size_t FAT32FS::list_contents(const char *path, void* buf, size_t size, uint64_t *offset){
    if (!mbs || !buf || size < sizeof(u32)) return 0;
    path = seek_to(path, '/');

    u32 count_sectors = count_FAT(mbs->first_cluster_of_root_directory);
    
    f32file_entry *entry = walk_directory(count_sectors, mbs->first_cluster_of_root_directory, path, list_entries_handler);
    
    u32 filecluster = *path ? (read_unaligned16(&entry->hi_first_cluster) << 16) | read_unaligned16(&entry->lo_first_cluster) : mbs->first_cluster_of_root_directory;
    u32 fat_count = count_FAT(filecluster);
    
    sizedptr ptr = list_directory(fat_count, filecluster);

    if (!ptr.ptr || !ptr.size) {
        *(uint32_t*)buf = 0;
        if (offset) *offset = 0;
        return sizeof(uint32_t);
    }
    
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
    		if (offset) *offset = hash;
    		break;
    	}
    }

    *(uint32_t*)buf = count;
    kfree((void*)ptr.ptr, ptr.size);

    return (uintptr_t)write_ptr-(uintptr_t)buf;
}

bool FAT32FS::stat(const char *path, fs_stat *out_stat){
    // path = seek_to(path, '/');
    // uint32_t count = count_FAT(mbs->first_cluster_of_root_directory);
    // f32file_entry *entry = walk_directory(count, mbs->first_cluster_of_root_directory, path, );
    return false;//TODO: stat
}

bool FAT32FS::truncate(file *descriptor, size_t size){
    // irq_flags_t irq = irq_save_disable();
    // module_file *mfile = (module_file*)chashmap_get(open_files, &descriptor->id, sizeof(uint64_t));
    // if (!mfile || !mfile->name) {
    //     irq_restore(irq);
    //     return false;
    // }
    // const char* path = mfile->name;
    // path = seek_to(path, '/');
    // uint32_t count = count_FAT(mbs->first_cluster_of_root_directory);
    // return walk_directory(count, mbs->first_cluster_of_root_directory, path, );
    
    // new function to walk directory and overwrite fsentry
    
    return false;
}