#include "tmp_fs.h"
#include "string/string.h"
#include "console/kio.h"
#include "files/dir_list.h"
#include "files/vfs.h"

bool init_tmp(){
    make_entry("std", backing_virtual, entry_file, DATA_SIG_UNKNOWN, buffer_create(0x1000, buffer_opt_none));
    return true;
}

system_module tmp_mod = {
    .name = "tmp",
    .mount = "tmp",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_tmp,
    .fini = 0,
    .open = vfs_open,
    .read = vfs_read,
    .write = vfs_write,
    .close = 0,
    .getstat = vfs_stat,
    .readdir = vfs_list,
};