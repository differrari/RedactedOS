#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "input_keycodes.h"

string directories[16];
int dir_count;
string files[16];
int file_count;

int selected = 0;

draw_ctx ctx;

void on_entry(const char* path, const char* name){
    if (!strcmp(name,"..") || !strcmp(name,".")) return;
    files[file_count++] = string_from_const(name);
}

void refresh(){
    for (int i = 0; i < file_count; i++)
        string_free(files[i]);
    file_count = 0;
    traverse_directory(directories[dir_count-1].data, false, on_entry);
    
    fb_clear(&ctx, 0);
    for (int i = 0; i < file_count; i++){
        if (selected == i){
            fb_draw_string(&ctx, ">", 0, 10 + (i * 30), 3, 0xFFFFFFFF);
        }
        fb_draw_string(&ctx, files[i].data, 30, 10 + (i * 30), 3, 0xFFFFFFFF);   
    }
    commit_draw_ctx(&ctx);
}

void enter(const char *name){
    if (dir_count >= 16) return;
    string full_path;
    if (dir_count)
        full_path = string_format("%s/%s",directories[dir_count-1].data,name);
    else 
        full_path = string_from_const(name);
    if (strend(name, ".red") == 0){
        string elf = string_format("%S/%v.elf",full_path,(string_slice){(char*)name,strlen(name)-4});
        exec(elf.data, 0, 0);
        string_free(elf);
        halt(0);
    } else 
        directories[dir_count++] = full_path;
    selected = 0;
    refresh();
}

void pop_dir(){
    if (dir_count <= 0) return;
    string_free(directories[dir_count-1]);
    dir_count--;
    selected = 0;
    refresh();
}

int main(){
    print("Haigh a dhomhain");
    
    request_draw_ctx(&ctx);
    enter("/shared");
    
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
                enter(files[selected].data);
                refresh();
            }
            if (ev.key == KEY_BACKSPACE){
                pop_dir();
                refresh();
            }
            print("selected %i",selected);
        }
    }
    
    return 0;
}