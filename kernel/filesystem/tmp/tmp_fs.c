#include "tmp_fs.h"
#include "string/string.h"
#include "console/kio.h"
#include "files/dir_list.h"
#include "files/vfs.h"

bool init_tmp(){
    make_entry("std", backing_virtual, entry_file, DATA_SIG_UNKNOWN, buffer_create(0x1000, buffer_opt_none));
    return true;
}

void tmp_create_container(string_slice bundle_id){
    string s = string_from_slice(bundle_id);
    size_t count = stack_count(entries);
    for (u64 i = 0; i < count; i++){
        module_file *file = stack_get(entries,i);
        if (!file) continue;
        if (string_equals(s, file->name)){
            string_free(s);
            return;
        }
    }
    make_entry(s.data, backing_virtual, entry_directory, 0, (buffer){});
}

system_module tmp_mod = {
    .name = "tmp",
    .mount = "ktmp",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_tmp,
    .fini = 0,
    .open = vfs_open,
    .read = vfs_read,
    .write = vfs_write,
    .create = vfs_create,
    .close = 0,
    .getstat = vfs_stat,
    .readdir = vfs_readdir,
};