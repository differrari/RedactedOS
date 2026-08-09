#include "procfs.h"
#include "files/dir_list.h"
#include "process.h"
#include "exceptions/irq.h"
#include "console/kio.h"
#include "scheduler.h"
#include "math/math.h"

extern process_t *process_list;
hash_map_t *proc_opened_files;

typedef struct {
    process_t *proc;
    uint16_t pid;
} procfs_owner;

void* proc_page;

void* procfs_alloc(size_t size){
    return allocate(proc_page, size, page_alloc);
}

bool init_procfs(){
    proc_page = page_alloc(PAGE_SIZE*16);
    if (!proc_opened_files) {
        proc_opened_files = hash_map_create(1024);
        proc_opened_files->free = release;
        proc_opened_files->alloc = procfs_alloc;
    }
    return true;
}

size_t list_processes(void *buf, size_t size, file_offset *offset){

    if (!buf || !offset || size < sizeof(uint32_t)) return 0;
	
	fs_dir_list_helper helper = create_dir_list_helper(buf, size);
    
	process_t *proc = process_list;
    if (*offset) {
        while (proc && proc->id != *offset) proc = proc->process_next;
    }

    while (proc) {
        if (proc->id != 0 && proc->state != STOPPED) {
            char name[6];
            string_format_buf(name, 6, "%i", proc->id);

            if (!dir_list_fill(&helper, name)){
                if (offset){ 
                    *offset = proc->id;
                    return dir_buf_size(&helper);
                }
            }
        }
        proc = proc->process_next;
    }

    return dir_buf_size(&helper);
}

#define NUM_PROC_FILES 2

char* proc_files[NUM_PROC_FILES] = {
    "out",
    "state"
};

size_t list_proc_files(void *buf, size_t size, file_offset *offset){

    if (!buf || !offset || size < sizeof(uint32_t)) return 0;
	
	fs_dir_list_helper helper = create_dir_list_helper(buf, size);
    
	u64 index = offset ? *offset : 0;
	
	for (int i = index; i < NUM_PROC_FILES; i++){
	    kprint(proc_files[i]);
		char *name = (char*)((uptr)helper.list + 4 + helper.offset);
	    if (!dir_list_fill(&helper, proc_files[i])){
			if (offset) *offset = i;
			return dir_buf_size(&helper);
		}
		kprint(name);
	}
    return dir_buf_size(&helper);
}

size_t readdir_proc(const char *path, void *buf, size_t size, file_offset *offset){
    irq_flags_t irq = irq_save_disable();
    if (!strlen(path)){
        size_t res = list_processes(buf, size, offset);
        irq_restore(irq);
        return res;
    }
    const char *pid_s = seek_to(path, '/');
    path = seek_to(pid_s, '/');
    uint64_t pid = parse_int_u64(pid_s, path - pid_s);
    process_t *proc = get_proc_by_pid(pid);
    if (!proc) {
        irq_restore(irq);
        return false;
    }
    if (!strlen(path)){
        size_t res = list_proc_files(buf, size, offset);
        irq_restore(irq);
        return res;
    }
    irq_restore(irq);
    return false;
}

FS_RESULT open_proc(const char *path, file *descriptor){
    uint64_t fid = reserve_fd_gid(path);
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)hash_map_get(proc_opened_files, &fid, sizeof(uint64_t));
    if (mfile){
        descriptor->id = mfile->fid;
        descriptor->size = mfile->file_size;
        descriptor->cursor = 0;
        mfile->references++;
        procfs_owner *owner_info = (procfs_owner*)mfile->private_data;
        if (owner_info && owner_info->proc && owner_info->proc->id == owner_info->pid) owner_info->proc->procfs_refs++;
        irq_restore(irq);
        return FS_RESULT_SUCCESS;
    }
    const char *pid_s = seek_to(path, '/');
    path = seek_to(pid_s, '/');
    uint64_t pid = parse_int_u64(pid_s, path - pid_s);
    process_t *proc = get_proc_by_pid(pid);
    if (!proc) {
        irq_restore(irq);
        return FS_RESULT_NOTFOUND;
    }
    descriptor->id = fid;
    descriptor->cursor = 0;
    module_file *file = procfs_alloc(sizeof(module_file));
    if (!file) {
        irq_restore(irq);
        return FS_RESULT_DRIVER_ERROR;
    }
    procfs_owner *owner_info = procfs_alloc(sizeof(procfs_owner));
    if (!owner_info) {
        irq_restore(irq);
        release(file);
        return FS_RESULT_DRIVER_ERROR;
    }
    owner_info->proc = proc;
    owner_info->pid = proc->id;
    file->fid = fid;
    file->private_data = owner_info;
    file->references = 1;
    if (strcmp_case(path, "out",true) == 0){
        descriptor->size = proc->output ? proc->output_size : proc->postmortem_output_size;
        file->read_only = true;
        file->buf = (uptr)(proc->output ? proc->output : proc->postmortem_output);
        file->file_buffer = (buffer){
            .buffer = (char*)(proc->output ? proc->output : proc->postmortem_output),
            .buffer_size = proc->output ? proc->output_size : proc->postmortem_output_size,
            .limit = proc->output ? PROC_OUT_BUF : proc->postmortem_output_size,
            .options = proc->output ? buffer_circular : buffer_static,
            .cursor = proc->output ? proc->output_size : 0,
        };
        proc->procfs_refs++;
    } else if (strcmp_case(path, "state",true) == 0){
        descriptor->size = sizeof(proc->state);
        file->read_only = true;
        file->buf = (uptr)&proc->state;
        file->file_buffer = (buffer){
            .buffer = (char*)&proc->state,
            .limit = sizeof(proc->state),
            .options = buffer_static,
            .buffer_size = sizeof(proc->state),
            .cursor = 0,
        };
        proc->procfs_refs++;
    } else {
        irq_restore(irq);
        release((void*)owner_info);
        release(file);
        return FS_RESULT_NOTFOUND;
    }
    file->file_size = descriptor->size;
    int put = hash_map_put(proc_opened_files, &descriptor->id, sizeof(uint64_t), file);
    irq_restore(irq);
    if (put >= 0) return FS_RESULT_SUCCESS;
    if ((uintptr_t)file->file_buffer.buffer == (uintptr_t)proc->output || (uintptr_t)file->file_buffer.buffer == (uintptr_t)proc->postmortem_output || (uintptr_t)file->file_buffer.buffer == (uintptr_t)&proc->state) {
        if (proc->procfs_refs) proc->procfs_refs--;
    }
    release((void*)owner_info);
    release(file);
    return FS_RESULT_DRIVER_ERROR;
}

bool stat_proc(const char *path, fs_stat *out_stat){
    if (!out_stat) return false;
    irq_flags_t irq = irq_save_disable();
    if (!strlen(path)){
        bool res = stat_dir(out_stat);
        irq_restore(irq);
        return res;
    }
    const char *pid_s = seek_to(path, '/');
    path = seek_to(pid_s, '/');
    uint64_t pid = parse_int_u64(pid_s, path - pid_s);
    process_t *proc = get_proc_by_pid(pid);
    if (!proc) {
        irq_restore(irq);
        return false;
    }
    if (!strlen(path)){
        bool res = stat_dir(out_stat);
        irq_restore(irq);
        return res;
    }
    out_stat->type = entry_file;
    if (strcmp_case(path, "out",true) == 0){
        out_stat->size = proc->output_size;
        out_stat->data_type = DATA_SIG_TEXT;
    }
    if (strcmp_case(path, "state",true) == 0){
        out_stat->size = sizeof(proc->state);
        out_stat->data_type = DATA_SIG_PROC_ST;
    }
    irq_restore(irq);
    return true;
}

int find_open_proc_file(void *node, void* key){
    uint64_t *fid = (uint64_t*)key;
    module_file *file = (module_file*)node;
    if (file->fid == *fid) return 0;
    return -1;
}

int find_open_proc_file_buffer(void *node, void* key){
    uintptr_t *buf = (uintptr_t*)key;
    module_file *file = (module_file*)node;
    if ((uintptr_t)file->file_buffer.buffer == *buf) return 0;
    return -1;
}

size_t read_proc(file* fd, char *buf, size_t size, file_offset offset){
    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    irq_flags_t irq = irq_save_disable();
    module_file *file = (module_file*)hash_map_get(proc_opened_files, &fd->id, sizeof(uint64_t));
    if (!file) {
        irq_restore(irq);
        return 0;
    }
    size_t s = buffer_read(&file->file_buffer, buf, size, offset);
    fd->size = file->file_size;
    irq_restore(irq);
    return s;
}

size_t write_proc(file* fd, const char *buf, size_t size, file_offset offset){
    process_t *proc = get_current_proc();
    if (fd->id == FD_OUT){
        if (!proc || !size) return 0;
        if (!proc->output) {
            proc->output = (kaddr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true);
            if (!proc->output) return 0;
        }
        irq_flags_t irq = irq_save_disable();

        buffer file_buffer = {
            .buffer = (char*)proc->output,
            .buffer_size = proc->output_size,
            .limit = PROC_OUT_BUF,
            .options = buffer_circular,
            .cursor = proc->output_size,
        };

        size = min(size, file_buffer.limit);
        size_t written = buffer_write_lim(&file_buffer, buf, size);

        proc->output_size = file_buffer.buffer_size;
        fd->size = proc->output_size;

        if (proc_opened_files){
            char fullpath[48] = {};
            string_format_buf(fullpath, sizeof(fullpath), "/%i/out", proc->id);
            uint64_t fid = reserve_fd_gid(fullpath);
            module_file *file = (module_file*)hash_map_get(proc_opened_files, &fid, sizeof(fid));
            if (file) {
                file->buf = (uptr)proc->output;
                file->file_buffer.buffer = (char*)proc->output;
                file->file_buffer.buffer_size = proc->output_size;
                file->file_buffer.limit = PROC_OUT_BUF;
                file->file_buffer.cursor = proc->output_size;
                file->file_buffer.options = buffer_circular;
                file->file_size = proc->output_size;
            }
        }

        irq_restore(irq);
        return written;
    }

    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    irq_flags_t irq = irq_save_disable();
    module_file *file = (module_file*)hash_map_get(proc_opened_files, &fd->id, sizeof(uint64_t));
    bool ro = file && file->read_only;
    irq_restore(irq);
    if (!file) return 0;
    if (ro) return 0;
    return 0;
}

extern bool process_can_reset(process_t *proc);
extern bool process_has_runtime_state(process_t *proc);

void close_proc(file *fd) {
    if (!fd) return;
    if (!proc_opened_files) return;

    uint64_t fid = fd->id;
    process_t *reset_proc = 0;
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)hash_map_get(proc_opened_files, &fid, sizeof(fid));
    if (!mfile) {
        irq_restore(irq);
        return;
    }

    procfs_owner *owner_info = (procfs_owner*)mfile->private_data;
    process_t *owner = 0;
    if (owner_info && owner_info->proc && owner_info->proc->id == owner_info->pid) owner = owner_info->proc;
    if (owner) {
        if (owner->procfs_refs) owner->procfs_refs--;
        if (process_can_reset(owner) && process_has_runtime_state(owner)) reset_proc = owner;
    }

    if (mfile->references > 0) mfile->references--;
    if (mfile->references == 0) {
        void *owned = mfile->file_buffer.buffer;
        buffer_options options = mfile->file_buffer.options;
        bool owned_postmortem = owner && owned == (void*)owner->postmortem_output;
        hash_map_remove(proc_opened_files, &fid, sizeof(fid), 0);
        irq_restore(irq);
        if (owned && options == buffer_opt_none && !(reset_proc && owned_postmortem)) {
            release(owned);
            if (owned_postmortem) {
                owner->postmortem_output = 0;
                owner->postmortem_output_size = 0;
            }
        }
        if (mfile->private_data) release(mfile->private_data);
        release(mfile);
        if (reset_proc) reset_process(reset_proc);
        return;
    }
    irq_restore(irq);
}

system_module procfs_mod = (system_module){
    .name = "scheduler",
    .mount = "proc",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_procfs,
    .fini = 0,
    .open = open_proc,
    .read = read_proc,
    .write = write_proc,
    .close = close_proc,
    .getstat = stat_proc,
    .readdir = readdir_proc,
};
