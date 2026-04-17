#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "input_keycodes.h"

string files[16];
int file_count;

int selected = 0;

draw_ctx ctx;

struct {
    color background;
    color accent;
} color_theme;

void on_entry(const char* path, const char* name){
    if (strend(name, ".config")) return;
    files[file_count++] = string_from_const(name);
}

void refresh(){
    for (int i = 0; i < file_count; i++)
        string_free(files[i]);
    file_count = 0;
    traverse_directory("/boot/redos/", false, on_entry);
    
    sreadf("/theme", &color_theme, sizeof(color_theme));
    
    fb_clear(&ctx, color_theme.background);
    for (int i = 0; i < file_count; i++){
        if (selected == i){
            fb_draw_string(&ctx, ">", 0, 10 + (i * 30), 3, color_theme.accent);
        }
        fb_draw_string(&ctx, files[i].data, 30, 10 + (i * 30), 3, color_theme.accent);   
    }
    commit_draw_ctx(&ctx);
}

int main(){
    request_draw_ctx(&ctx);
    
    refresh();
    
    while (true){
        kbd_event ev = {};
        if (read_event(&ev)){
            if (ev.type != KEY_PRESS) continue;
            if (ev.key == KEY_ESC) return 0;
            if (ev.key == KEY_UP){
                selected = (selected - 1 + file_count) % file_count;
                refresh();
            }
            if (ev.key == KEY_DOWN){
                selected = (selected + 1 + file_count) % file_count;
                refresh();
            }
            if (ev.key == KEY_ENTER){
                //TODO: need to consider how to make writes overwrite instead of append, and wipe the entire file
                swritef("/shared/theme", files[selected].data, files[selected].length+1);
                swritef("/theme/reload", 0, 0);
                refresh();
            }
        }
    }
    
    return 0;
}