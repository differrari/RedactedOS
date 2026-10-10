#ifdef FEATURE_NEW_PROCFS

#include "files/system_module.h"
#include "files/folderfs.h"
#include "scheduler.h"
#include "console/kio.h"

typedef enum { procfs_type_none, procfs_type_id, procfs_type_in, procfs_type_out, procfs_type_state, procfs_type_info } procfs_data_types;

typedef struct {
    u16 procid;
} proc_data;

void procfs_list(void *ctx, u64 original_index, u64 *out_offset){
    proc_data *data = ctx;
    string s = string_format("%i",data->procid);
    if (!dir_list_fill(router_fs_dir_helper, s.data)){
        if (out_offset) *out_offset = current_offset;
        string_free(s);
        return;
    }
    string_free(s);
} 

static inline buffer procfs_make_info_buffer(process_t *proc){
    return buffer_create(sizeof(proc_info), buffer_static);
}

FS_RESULT procfs_open_output(process_t *proc, file *fd){
    fd->id = ((procfs_type_out & 0xFFFF) << 16) | proc->id;
    fd->data_type = DATA_SIG_TEXT;
    if (!proc->procfs.output.buffer_size){
        proc->procfs.output = (buffer){
            .buffer = (char*)(proc->output ? proc->output : proc->postmortem_output),
            .buffer_size = proc->output ? proc->output_size : proc->postmortem_output_size,
            .limit = proc->output ? PROC_STDIO_BUF : proc->postmortem_output_size,
            .options = proc->output ? buffer_circular : buffer_static,
            .cursor = proc->output ? proc->output_size : 0,
        };
    }
    fd->size = proc->procfs.output.buffer_size;
    return FS_RESULT_SUCCESS;
}

FS_RESULT procfs_open_input(process_t *proc, file *fd){
    fd->id = ((procfs_type_in & 0xFFFF) << 16) | proc->id;
    fd->data_type = DATA_SIG_TEXT;
    if (!proc->procfs.input.buffer_size && proc->input){
        proc->procfs.input = (buffer){
            .buffer = (char*)proc->input,
            .buffer_size = proc->input_size,
            .limit = PROC_STDIO_BUF,
            .options = buffer_circular,
            .cursor = proc->input_size,
        };
    }
    fd->size = proc->procfs.output.buffer_size;
    return FS_RESULT_SUCCESS;
}

FS_RESULT procfs_open(u64 id, string_slice file_name, file *fd){
    process_t *proc = get_proc_by_pid(id);
    if (!proc) return FS_RESULT_NOTFOUND;
    process_t *cproc = get_current_proc();
    if (slice_lit_match(file_name, "out", true)){
        if (proc != cproc && !auth_valid(auth_get_proc_token(cproc), auth_process_output, auth_map_process(proc)))
            return FS_RESULT_NOTFOUND;
        return procfs_open_output(proc, fd);
    }
    if (slice_lit_match(file_name, "state", true)){
        if (proc != cproc && !auth_valid(auth_get_proc_token(cproc), auth_process_state, auth_map_process(proc))){
            print("Auth failure for reading state");
            return FS_RESULT_NOTFOUND;
        }
        fd->id = ((procfs_type_state & 0xFFFF) << 16) | id;
        fd->data_type = DATA_SIG_PROC_ST;
        fd->size = sizeof(process_state);
        if (!proc->procfs.state.buffer_size){
            buffer_map_value(&proc->procfs.state, &proc->state, sizeof(process_state), DATA_SIG_PROC_ST);
        }
        return FS_RESULT_SUCCESS;
    }
    if (slice_lit_match(file_name, "info", true)){
        if (proc != cproc && !auth_valid(auth_get_proc_token(cproc), auth_process_info, auth_map_process(proc))){
            print("Auth failure for reading info");
            return FS_RESULT_NOTFOUND;
        }
        fd->id = ((procfs_type_info & 0xFFFF) << 16) | id;
        fd->data_type = DATA_SIG_PROC_INFO;
        fd->size = sizeof(proc_info);
        if (!proc->procfs.info.buffer_size){
             proc->procfs.info = procfs_make_info_buffer(proc);
        }
        return FS_RESULT_SUCCESS;
    }
    if (slice_lit_match(file_name, "in", true)){
        if (!auth_valid(auth_get_proc_token(cproc), auth_process_input, auth_map_process(proc)))
            return FS_RESULT_NOTFOUND;
        return procfs_open_input(proc, fd);
    }
    return FS_RESULT_NOTFOUND;
}

void procfs_fill_info(process_t *proc, buffer *buf){
    size_t expected_size = sizeof(proc_info);
    if (!buf || !buf->buffer || buf->limit < expected_size){
        if (buf) buffer_destroy(buf);
        proc->procfs.info = procfs_make_info_buffer(proc);
    }
    size_t namelen = strlen(proc->name);
    if (namelen >= MAX_PROC_NAME_LENGTH) namelen = MAX_PROC_NAME_LENGTH-1;
    proc_info info = {
        .id = proc->id,
        .procnamelen = namelen,
        .stack = {proc->main_thread.stack_info.top,proc->main_thread.stack_info.size},
        .sp = proc->main_thread.sp,
        .heap = {proc->mm.mmap_bottom,proc->mm.mmap_top-proc->mm.mmap_bottom},
        .pc = proc->main_thread.pc,
        .privilege = is_privileged(proc),
        .state = proc->state
    };
    memcpy(info.procname, proc->name, namelen);
    buffer_write_lim(buf, (char*)&info, expected_size);
}

buffer* procfs_resolve_fd(file *fd){
    if (fd->id == FD_OUT){
        process_t *proc = get_current_proc();
        if (!proc) return 0;
        procfs_open_output(proc, fd);
        return &proc->procfs.output;
    }
    if (fd->id == FD_IN){
        process_t *proc = get_current_proc();
        if (!proc) return 0;
        procfs_open_input(proc, fd);
        return &proc->procfs.input;
    }
    u16 procid = fd->id & 0xFFFF;
    process_t *proc = get_proc_by_pid(procid);
    if (!proc) return 0;
    u16 file_type = (fd->id >> 16) & 0xFFFF;
    switch (file_type){
        case procfs_type_out:
            return &proc->procfs.output;
        case procfs_type_state:
            return &proc->procfs.state;
        case procfs_type_info:
            procfs_fill_info(proc, &proc->procfs.info);
            return &proc->procfs.info;
        case procfs_type_id:
            return &proc->procfs.id;
        case procfs_type_in:
            return &proc->procfs.input;
        default: return 0;
    }
    return 0;
}

u64 resolve_reserved_fd(u64 fd){
    if (fd == FD_OUT){
        return ((procfs_type_out & 0xFFFF) << 16) | get_current_proc_pid();
    }
    if (fd == FD_IN){
        return ((procfs_type_in & 0xFFFF) << 16) | get_current_proc_pid();
    }
    return 0;
}

bool init_procfs(){
    if (!folderfs_init()) return false;
    folderfs_custom_list = procfs_list;
    folderfs_custom_open = procfs_open;
    folderfs_resolve_fd = procfs_resolve_fd;
    static_entries += make_entry("id", backing_virtual, entry_file, DATA_SIG_RAW, (buffer){}) != 0;
    static_entries += make_entry(":id/in", backing_virtual, entry_file, DATA_SIG_RAW, (buffer){}) != 0;
    static_entries += make_entry(":id/out", backing_virtual, entry_file, DATA_SIG_TEXT, (buffer){}) != 0;
    static_entries += make_entry(":id/state", backing_virtual, entry_file, DATA_SIG_PROC_ST, (buffer){}) != 0;
    static_entries += make_entry(":id/info", backing_virtual, entry_file, DATA_SIG_PROC_INFO, (buffer){}) != 0;
    return true;
}

void register_procfs(u16 procid){
    process_t *proc = get_proc_by_pid(procid);
    if (!proc) return;
    proc->output = (kaddr_t)palloc(PROC_STDIO_BUF, MEM_PRIV_KERNEL, MEM_RW, true);//TODO: only do this when it's written to
    proc->input = (kaddr_t)palloc(PROC_STDIO_BUF, MEM_PRIV_KERNEL, MEM_RW, true);//TODO: only do this when it's written to
    proc_data *data = zalloc(sizeof(proc_data));
    data->procid = procid;
    folderfs_create_folder(data);
}

static inline FS_RESULT procfs_main_open(const char *path, file *fd){
    if (strncmp(path,"/id", 3) == 0){
        process_t *proc = get_current_proc();
        fd->id = ((procfs_type_id & 0xFFFF) << 16) | proc->id;
        fd->data_type = DATA_SIG_RAW;
        fd->size = sizeof(u16);
        if (!proc->procfs.id.buffer_size){
            buffer_map_value(&proc->procfs.id, &proc->id, sizeof(u16), DATA_SIG_RAW);
        }
        return FS_RESULT_SUCCESS;
    }
    return folderfs_open(path, fd);
}

system_module procfs_mod = (system_module){
    .name = "procfs",
    .mount = "proc",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_procfs,
    .fini = 0,
    .open = procfs_main_open,
    .read = folderfs_read,
    .write = folderfs_write,
    // .close = folderfs_close,
    .getstat = folderfs_stat,
    .readdir = folderfs_readdir,
};

#endif