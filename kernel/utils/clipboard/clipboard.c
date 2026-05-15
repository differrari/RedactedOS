#include "clipboard.h"
#include "data/struct/ring_buffer.h"
#include "files/buffer.h"
#include "syscalls/syscalls.h"
#include "files/stack_fs.h"
#include "console/kio.h"

system_module clipboard_mod = {
    .name = "clipboard",
    .mount = "clipboard",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = stackfs_init,
    .open = stackfs_open,
    .read = stackfs_read,
    .write = stackfs_write,
    .readdir = stackfs_readdir,
    .getstat = stackfs_stat
};