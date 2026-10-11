#include "sync.h"
#define MOG_MODULE "FSSYNC"
#define MOG_LEVEL MOG_VERBOSE
#include "utils/mog.h"
#include "syscalls/syscalls.h"

typedef struct {
    u64 time;
    range_t r;
} modification_entry;

#ifdef SYNCFD_PROTOTYPE
modification_entry hardcoded_mod_history[128];
i32 mod_history_count;
#endif

void sync_fd(file *ifd, open_file_descriptors *ofile){
#ifdef SYNCFD_PROTOTYPE
    if (ofile->resource_id.resource == 0x1502){
        mog(VERBOSE,"There are %i modifications",mod_history_count);
        for (i32 i = 0; i < mod_history_count; i++){
            if (hardcoded_mod_history[i].time > ofile->last_sync){
                if (hardcoded_mod_history[i].r.start <= ifd->cursor || hardcoded_mod_history[i].r.start + hardcoded_mod_history[i].r.size <= ifd->cursor){
                    ifd->cursor += hardcoded_mod_history[i].r.size;
                    ifd->size += hardcoded_mod_history[i].r.size;
                    mog(VERBOSE,"Moved fd %llx by %i",ifd->id,hardcoded_mod_history[i].r.size);
                }
            }
        }
    }
#endif
    ofile->last_sync = get_time();
}

// Only keep track of modifications so long as there is a single IFD that wants to synchronize itself
// For each IFD that wants synchronization, only maintain changes starting at the lowest last_sync_time
// Timeout changes. Any last_sync_time longer than n will require manual-resyncing

// OR

// Instead of keeping track of individual changes, keep track of synced cursors
// As a mod is made, keep track of the movement of the cursor
// Sync to each cursor once requested

void report_fd_mod(file *ifd, open_file_descriptors *ofile, u64 time, range_t range){
#ifdef SYNCFD_PROTOTYPE
    if (ofile->resource_id.resource == 0x1502 && mod_history_count < 128){
        hardcoded_mod_history[mod_history_count++] = (modification_entry){
            .time = time,
            .r = range,
        };
        mog(VERBOSE,"Registered fd %llx modification at %r",ifd->id,range);
    }
#endif
}