#include "folderfs.h"
#include "console/kio.h"

bool env_loaded = false;

typedef struct {
    u16 procid;
} env_data;

void environment_list(void *ctx, u64 original_index, u64 *out_offset){
    env_data *data = ctx;
    string s = string_format("%i",data->procid);
    if (!dir_list_fill(router_fs_dir_helper, s.data)){
        if (out_offset) *out_offset = current_offset;
        string_free(s);
        return;
    }
    string_free(s);
} 

FS_RESULT environment_open(u64 id, string_slice file_name, file *fd){
    process_t *proc = get_proc_by_pid(id);
    if (!proc) return FS_RESULT_NOTFOUND;
    if (slice_lit_match(file_name, "display", true)){
        fd->id = ((env_type_display & 0xFFFF) << 16) | id;
        fd->data_type = DATA_SIGNATURE("OUTFMT");
        fd->size = sizeof(env_display_type);
        if (!proc->environment.display_buf.buffer){
            buffer_map_value(&proc->environment.display_buf,&proc->environment.display_type,sizeof(proc->environment.display_type), fd->data_type);
        }
        return FS_RESULT_SUCCESS;
    }
    return FS_RESULT_NOTFOUND;
}

buffer* environment_resolve_fd(file *fd){
    u16 procid = fd->id & 0xFFFF;
    process_t *proc = get_proc_by_pid(procid);
    if (!proc) return 0;
    u16 file_type = (fd->id >> 16) & 0xFFFF;
    switch (file_type){
        case env_type_display:
            return &proc->environment.display_buf;
        default: return 0;
    }
    return 0;
}

bool environment_init(system_module *module){
    if (!folderfs_init()) return false;
    folderfs_custom_list = environment_list;
    folderfs_custom_open = environment_open;
    folderfs_resolve_fd = environment_resolve_fd;
    static_entries += make_entry(":id/display", backing_virtual, entry_file, DATA_SIGNATURE("OUTFMT"), (buffer){}) != 0;
    return true;
}

void register_environment(u16 procid){
    env_data *data = zalloc(sizeof(env_data));
    data->procid = procid;
    folderfs_create_folder(data);
}

system_module environment_module = {
    .name = "environment",
    .mount = "environments",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = environment_init,
    .read = folderfs_read ,
    .write = folderfs_write,
    .getstat = folderfs_stat,
    .readdir = folderfs_readdir,
    .open = folderfs_open,
};