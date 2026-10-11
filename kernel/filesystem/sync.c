#include "sync.h"
#define MOG_MODULE "FSSYNC"
#define MOG_LEVEL MOG_VERBOSE
#include "utils/mog.h"
#include "syscalls/syscalls.h"
#include "data/struct/linked_list.h"

hash_map_t *synced_files;

void sync_fd(file *ifd, open_file_descriptors *ofile, bool replace){
    if (ifd->sync_type == cursor_sync_none) return;
    if (!synced_files) synced_files = hash_map_create(64);
    
    if (ifd->sync_type == cursor_sync_head){
        mog(VERBOSE,"File %lli moved to head",ifd->id);
        ifd->cursor = 0;
        return;
    }
    if (ifd->sync_type == cursor_sync_tail){
        mog(VERBOSE,"File %lli moved to tail",ifd->id);
        ifd->cursor = ofile->file_size;
        return;
    }
    
    hash_map_t *cursors = hash_map_get(synced_files, &ofile->resource_id, sizeof(resource));
    if (!cursors){
        cursors = hash_map_create(16);
        hash_map_put(synced_files, &ofile->resource_id, sizeof(resource), cursors);
    }
    
    u64 loc = 0;
    if (hash_map_has_key(cursors, &ifd->id, sizeof(u64), (void**)&loc)){
        mog(VERBOSE,"Syncing cursor for %lli from %lli to %lli",ifd->id,ifd->cursor,loc);
        if (replace){
            hash_map_remove(cursors, &ifd->id, sizeof(u64), 0);
            hash_map_put(cursors, &ifd->id, sizeof(u64), (void*)ifd->cursor);
        } else {
            ifd->cursor = loc;
        }
    } else {
        hash_map_put(cursors, &ifd->id, sizeof(u64), (void*)ifd->cursor);
    }
    
    ofile->last_sync = get_time();
    ifd->size = ofile->file_size;
}

void report_fd_mod(file *ifd, open_file_descriptors *ofile, u64 time, range_t range){
    if (!synced_files) return;
    hash_map_t *cursors = hash_map_get(synced_files, &ofile->resource_id, sizeof(resource));
    if (!cursors) return;
    for (uint64_t i = 0; i < cursors->capacity; i++){
        hash_map_entry_t* e = cursors->buckets[i];
        while(e){
            hash_map_entry_t* next = e->next;
            u64 loc = (u64)e->value;
            if (range.start <= loc) loc += range.size;
            e->value = (void*)loc;
            e = next;
        }
    }
}