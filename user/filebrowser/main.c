#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "input_keycodes.h"
#include "data/struct/stack.h"
#include "header_utils/filebrowser.h"
#include "files/vfs.h"

size_t test_fn(file *fd, const char *c, size_t s, file_offset off){
    print("Test button pressed");
    return 0;
}

void menu_init(){
    make_entry("File", backing_virtual, entry_directory, 0, (buffer){});
    make_entry("File" "/" "Hello", backing_virtual, entry_file, 0, (buffer){});
    make_entry("Edit", backing_virtual, entry_directory, 0, (buffer){});
    make_entry("Edit/World", backing_virtual, entry_directory, 0, (buffer){});
    make_complex_entry("File/Test", backing_transform, entry_file, 0, (file_actions){.write = test_fn}, (string){});
}

system_module menu_mod = {
    .name = "demo menu",
    .mount = "menu",
    //TODO: can init be brought back now?
    .version = VERSION_NUM(0, 1, 0, 0),
    .open = vfs_open,
    .read = vfs_read,
    .write = vfs_write,
    .getstat = vfs_stat,
    .readdir = vfs_readdir,
};

int main(){
    menu_init();
    load_fsmodule(&menu_mod, true);
    request_draw_ctx(&filebrowser_ctx);
    
    files = stack_create(sizeof(file_data),32);
    directories = stack_create(sizeof(string),16);
    
    enter("/");

    while (true){
        commit_draw_ctx(&filebrowser_ctx);
        kbd_event ev = {};
        if (!read_event(&ev)) {
            msleep(25);
            continue;
        }
        if (ev.type != KEY_PRESS) continue;
        if (ev.key == KEY_ESC) return 0;
        filebrowser_input(ev);
    }

    return 0;
}
