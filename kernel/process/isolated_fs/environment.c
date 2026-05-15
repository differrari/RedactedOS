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

bool environment_init(system_module *module){
    if (!folderfs_init()) return false;
    folderfs_custom_list = environment_list;
    static_entries += make_entry(":id/format", backing_virtual, entry_file, DATA_SIGNATURE("OUTFMT"), (buffer){}) != 0;
    kprint("Environment done");
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
    // .read = stackfs_read ,
    // .write = stackfs_write,
    .getstat = folderfs_stat,
    .readdir = folderfs_readdir,
    // .open = stackfs_open,
};