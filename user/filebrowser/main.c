#include "syscalls/syscalls.h"
#include "files/helpers.h"
#include "input_keycodes.h"
#include "data/struct/stack.h"

arr_stack_t *directories;
arr_stack_t *files;

int selected = 0;

bool viewing_file = false;
char *viewed_path;
char *viewed_data;
size_t viewed_size;

draw_ctx ctx;

bool disable_multiverse;

typedef struct {
    string name;
    string full_path;
    fs_entry_type type;
    data_signature signature;
} file_data;

//TODO: preferences/settings system
//TODO: put in library
//TODO: design a system-wide method for opening files by extension

void on_entry(const char* path, const char* name){
    if (!strcmp(name, "..") || !strcmp(name, ".")) return;
    string full_path;
    u64 dir_count = stack_count(directories);
    if (dir_count){
        string parent = STACK_GET(string,directories,dir_count-1);
        if (parent.data[parent.length-1] == '/')
            full_path = string_format("%s%s", parent.data, name);
        else     
            full_path = string_format("%s/%s", parent.data, name);
    } else
        full_path = string_from_const(name);
    string s = string_from_const(name);
    file_data fdata = {
        .full_path = full_path,
        .name = s,
    };
    fs_stat st = {};
    if (statf(full_path.data, &st)){
        fdata.signature = st.data_type;
        fdata.type = st.type;
    }
    stack_push(files, &fdata);
}

color color_for_type(fs_entry_type type){
    switch (type) {
        case entry_directory: return 0xFF0000DE;
        case entry_invalid: return 0xFF777777;
        default: return 0xFFFFFFFF;
    }
}

void refresh(){
    for (u64 i = 0; i < stack_count(files); i++)
        string_free(STACK_GET(string,files,i));
    stack_reset(files);

    u64 dir_count = stack_count(directories);
    if (dir_count > 0)
        traverse_directory(STACK_GET(string,directories,dir_count-1).data, false, on_entry);
    
    i64 file_count = stack_count(files);

    if (selected >= file_count)
        selected = file_count > 0 ? file_count - 1 : 0;

    fb_clear(&ctx, 0);
    if (viewing_file){
        fb_draw_string(&ctx, viewed_path, 4, 4, 2, 0xFFFFFFFF);
        if (!viewed_data){
            fb_draw_string(&ctx, "Failed to open file", 4, 40, 2, 0xFFFFFFFF);
        } else {
            size_t probe = viewed_size < 512 ? viewed_size : 512;
            bool text = true;
            for (size_t i = 0; i < probe; i++){
                unsigned char c = (unsigned char)viewed_data[i];
                if (!c || (c < 0x20 && c != '\n' && c != '\r' && c != '\t')){
                    text = false;
                    break;
                }
            }

            if (!text){
                fb_draw_string(&ctx, "Binary file preview is not supported", 4, 40, 2, 0xFFFFFFFF);
            } else {
                int scale = 2;
                int char_w = (int)gpu_char_size(scale);
                int char_h = char_w + 2;
                int cols = ctx.width > 8 ? (int)(ctx.width - 8) / char_w : 1;
                int rows = ctx.height > 48 ? (int)(ctx.height - 48) / char_h : 1;
                int x = 4;
                int y = 40;
                int drawn = 0;
                size_t i = 0;
                if (cols < 1) cols = 1;
                if (rows < 1) rows = 1;
                while (i < viewed_size && drawn < rows){
                    char line[256];
                    int n = 0;
                    while (i < viewed_size && viewed_data[i] != '\n' && viewed_data[i] != '\r' && n < cols && n < (int)sizeof(line) - 1)
                        line[n++] = viewed_data[i++];
                    line[n] = 0;
                    fb_draw_string(&ctx, line, x, y, scale, 0xFFFFFFFF);
                    drawn++;
                    y += char_h;
                    while (i < viewed_size && (viewed_data[i] == '\n' || viewed_data[i] == '\r')) i++;
                }
            }
        }
        commit_draw_ctx(&ctx);
        return;
    }

    if (dir_count > 0)
        fb_draw_string(&ctx, STACK_GET(string,directories,dir_count - 1).data, 4, 4, 2, 0xFFFFFFFF);

    for (int i = 0; i < file_count; i++){
        int y = 40 + (i * 30);
        if (selected == i)
            fb_draw_string(&ctx, ">", 0, y, 3, 0xFFFFFFFF);
        file_data data = STACK_GET(file_data,files,i);
        int offset = 30;
        fb_draw_string(&ctx, data.name.data, offset, y, 3, color_for_type(data.type));
        offset += fb_get_char_size(3) * (data.name.length + 1);
        if (data.signature){
            bool valid = true;
            char *sig = (char*)&data.signature;
            for (int c = 0; c < 8; c++){
                valid &= !sig[c] || is_printable(sig[c]);
            }
            if (valid){
                fb_draw_slice(&ctx, (string_slice){sig,8}, offset, y, 3, 0xFF999999);
            }
        }
    }
    commit_draw_ctx(&ctx);
}

void enter_path(const char *name, const char *full_path){
    if (!name || !*name) return;

    if (strend(name, ".red") == 0){
        exec(full_path, 0, 0, EXEC_MODE_DEFAULT);
        halt(0);
    }
    
    if (strend(name, ".c") == 0){
        const char* argv[1] = { full_path }; 
        exec("/home/applications/braincode.red", 1, argv, EXEC_MODE_DEFAULT);
        halt(0);
    }

    fs_stat st = {};
    if (!statf(full_path, &st)){
        print("Failed to get info for %s",full_path);
        return;
    }

    if (st.type == entry_directory){
        string s = string_from_literal(full_path);
        stack_push(directories,&s);
        selected = 0;
        viewing_file = false;
        refresh();
        return;
    }

    if (viewed_data){
        release(viewed_data);
        viewed_data = 0;
    }
    viewed_path = (char*)full_path;
    viewed_data = read_full_file(viewed_path, &viewed_size);
    viewing_file = true;
    refresh();
}

void enter(char *name){
    enter_path(name, name);
}

void pop_dir(){
    if (viewing_file){
        if (viewed_data){
            release(viewed_data);
            viewed_data = 0;
        }
        if (viewed_path)
            viewed_path = 0;
        viewed_size = 0;
        viewing_file = false;
        refresh();
        return;
    }
    u64 dir_count = stack_count(directories);
    if (dir_count <= disable_multiverse) return;
    string last = STACK_GET(string, directories, dir_count-1);
    string_free(last);
    stack_remove(directories,1);
    selected = 0;
    refresh();
}

int main(){
    request_draw_ctx(&ctx);
    
    files = stack_create(sizeof(file_data),32);
    directories = stack_create(sizeof(string),16);
    
    enter("/");

    while (true){
        kbd_event ev = {};
        if (!read_event(&ev)) {
            msleep(25);
            continue;
        }
        if (ev.type != KEY_PRESS) continue;
        if (ev.key == KEY_ESC) return 0;
        if (ev.key == KEY_BACKSPACE){
            pop_dir();
            continue;
        }
        if (viewing_file)
            continue;
        if (!stack_count(files))
            continue;
        u64 file_count = stack_count(files);
        if (ev.key == KEY_UP){
            selected = (selected - 1 + file_count) % file_count;
            refresh();
            continue;
        }
        if (ev.key == KEY_DOWN){
            selected = (selected + 1) % file_count;
            refresh();
            continue;
        }
        if (ev.key == KEY_ENTER || ev.key == KEY_KPENTER){
            file_data data = STACK_GET(file_data,files,selected);
            enter_path(data.name.data,data.full_path.data);
        }
    }

    return 0;
}
