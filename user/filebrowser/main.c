#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "input_keycodes.h"
#include "data/struct/stack.h"
#include "header_utils/filebrowser.h"
#include "utils/menu.h"

size_t test_fn(const char *path, void *c, size_t s){
    print("Test button pressed");
    return 0;
}

MAKE_MENU({
    MENU_SUBMENU("File", {
        MENU_SUBMENU("Hello", {
            MENU_ITEM("World", 0)
        })
        MENU_ITEM("Test", test_fn)
    })
    MENU_SUBMENU("Edit", {
        MENU_ITEM("Delete", 0)
    })
})

int main(){
    menu_init();
    
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
