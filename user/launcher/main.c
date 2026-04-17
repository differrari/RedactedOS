#include "syscalls/syscalls.h"
#include "ui/uno/uno.h"
#include "math/math.h"
#include "files/helpers.h"
#include "data/struct/chunk_array.h"
#include "memory/memory.h"
#include "input_keycodes.h"
#include "string/slice.h"
#include "package_info.h"

#define MAX_COLS 3
#define MAX_ROWS 3

typedef struct {
    string_slice name;
    string_slice ext;
    string path;
    string file_name;
    package_info info;
} launch_entry;

void draw_full();
bool await_gpu();
void activate_current();
void draw_tile(uint32_t column, uint32_t row);

gpu_size tile_size;
gpu_point selected;
bool ready = false;
bool rendered_full = false;
file active_proc;
chunk_array_t *entries;
bool process_active = false;
draw_ctx ctx;
i32 bg_color;
i32 text_color;

void *launcher_page = 0;

void* alloc_launcher(size_t size){
    if (!launcher_page) {
        launcher_page = malloc(PAGE_SIZE);
    }
    return allocate(launcher_page, size, malloc);
}

void add_entry(string_slice name, string_slice ext, string path, package_info info){
    chunk_array_push(entries, &(launch_entry){
        .name = name,
        .ext = ext,
        .path = path,
        .info = info,
    });
}

uint16_t find_extension(char *path){
    uint16_t count = 0;
    while (*path && *path != '.'){ path++; count++; }
    return path ? count : 0;
}

package_info get_pkg_info(char* info_path){
    package_info pkg = {};
    char *info = read_full_file(info_path, 0);
    if (!info) return pkg;
    pkg = parse_package_info(info);
    release(info);
    return pkg;
}

void handle_entry(const char *directory, const char *file) {
    if (!strcmp_case("launcher.red",file,true)) return;
    string fullpath = string_format("%s/%s",directory, (uintptr_t)file);
    uint16_t ext_loc = find_extension((char*)file);
    string_slice name = make_string_slice(fullpath.data, fullpath.length - strlen(file), ext_loc);
    uint16_t extra = ext_loc ? 1 : 0;
    string_slice ext = make_string_slice(name.data + ext_loc + 1, 0, strlen(file)-ext_loc-extra);
    if (slice_lit_match(ext,"red",true)){
        string pkg_info = string_concat(fullpath, string_from_literal("/package.info"));
        add_entry(name, ext, fullpath, get_pkg_info(pkg_info.data));
        // string_free(pkg_info);
    }
}

void load_entries(){
    if (!entries)
        entries = chunk_array_create_alloc(sizeof(launch_entry), 9, alloc_launcher, 0);
    size_t c = chunk_array_count(entries);
    for (uint32_t i = 0; i < c; i++){
        // launch_entry *entry = chunk_array_get(entries, i);
        // string_free(entry->path);
        // string_free(entry->info.name);
        // string_free(entry->info.version);
        // string_free(entry->info.author);
    }
    chunk_array_reset(entries);
    traverse_directory("/shared/applications", false, handle_entry);
    traverse_directory("/boot/redos/system", false, handle_entry);
}

void draw_desktop(){
    if (active_proc.id){
        u16 state = 0;
        readf(&active_proc,(char*)&state, sizeof(state));
        if (state)
            return;
    } 
    if (process_active){
        active_proc.id = 0;
        load_entries();
        memset(&ctx, 0, sizeof(draw_ctx));
        request_draw_ctx(&ctx);
        if (ctx.width < 512 || ctx.height < 256){
            resize_draw_ctx(&ctx, max(512,ctx.width), max(256, ctx.height));
        }
        gpu_size screen_size = {ctx.width, ctx.height};
        tile_size = (gpu_size){ screen_size.width/MAX_COLS - 20, screen_size.height/(MAX_ROWS+1) - 20 };
        rendered_full = false;
        process_active = false;
    }
    gpu_point old_selected = selected;
    kbd_event event;
    while (read_event(&event)){
        if (event.type == KEY_PRESS){
            switch (event.key) {
                case KEY_ENTER:
                case KEY_KPENTER:
                    activate_current();
                    return;
                case KEY_RIGHT: selected.x = (selected.x + 1) % MAX_COLS; break;
                case KEY_LEFT: selected.x = (selected.x - 1 + MAX_COLS) % MAX_COLS; break;
                case KEY_DOWN: selected.y = (selected.y + 1 ) % MAX_ROWS; break;
                case KEY_UP: selected.y = (selected.y - 1 + MAX_ROWS) % MAX_ROWS; break;
            }
        }
    }
    if (!rendered_full){
        rendered_full = true;
        draw_full();
        ctx.full_redraw = true;
        commit_draw_ctx(&ctx);
    } else if (old_selected.x != selected.x || old_selected.y != selected.y){
        draw_tile(old_selected.x, old_selected.y);
        draw_tile(selected.x, selected.y);
        ctx.full_redraw = true;
        commit_draw_ctx(&ctx);
    }
}

void draw_full(){
    if (!await_gpu()) return;
    fb_clear(&ctx, bg_color+0x050505);
    for (uint32_t column = 0; column < MAX_COLS; column++){
        for (uint32_t row = 0; row < MAX_ROWS; row++){
            draw_tile(column, row);
        }
    }
}

bool await_gpu(){
    if (!ready){
        request_draw_ctx(&ctx);
        gpu_size screen_size = {ctx.width, ctx.height};
        tile_size = (gpu_size){screen_size.width/MAX_COLS - 20, screen_size.height/(MAX_ROWS+1) - 20};
        ready = true;
        u32 color_buf[2] = {};
        sreadf("/theme", &color_buf, sizeof(uint64_t));
        bg_color = color_buf[0];
        text_color = color_buf[1];
    }
    return ready;
}

void activate_current(){
    uint32_t index = (selected.y * MAX_COLS) + selected.x;

    if (index < chunk_array_count(entries)){
        launch_entry *entry = chunk_array_get(entries, index);
        if (!slice_lit_match(entry->ext, "red", true)){
            print("[LAUNCHER error] Wrong format %v. Must be a .red package",entry->ext);
            return;
        }
        string s = string_format("%s/%v.elf",entry->path.data, entry->name);
        fb_clear(&ctx, 0);
        commit_draw_ctx(&ctx);
        u16 pid = exec(s.data, 0, 0, EXEC_MODE_DEFAULT);
        string_free(s);
        if (!pid) {
            print("[LAUNCHER error] failed to launch process");
            return;
        }
        string p = string_format("/proc/%i/state",pid);
        if (openf(p.data, &active_proc) != FS_RESULT_SUCCESS){
            string_free(p);
            print("[LAUNCHER error] failed to get process state");
            return;
        }
        string_free(p);
        process_active = true;
    }
    
}

void draw_tile(uint32_t column, uint32_t row){
    bool sel = (u32)selected.x == column && (u32)selected.y == row;
    uint32_t index = (row * MAX_COLS) + column;
    
    int border = 4;

    gpu_rect inner_rect = (gpu_rect){{10 + ((tile_size.width + 10)*column)+ (sel ? border : 0), 50 + ((tile_size.height + 10) *row) + (sel ? border : 0)}, {tile_size.width - (sel ? border * 2 : 0), tile_size.height - (sel ? border * 2 : 0)}};

    DRAW(
        rectangle(&ctx, (rect_ui_config){
        .border_size = (uint8_t)(sel ? 4 : 0),
        .border_color = bg_color+0x333333,
        .border_padding = 0,
        }, (common_ui_config){
        .point = (int_point){10 + (int32_t)((tile_size.width + 10)*column), 50 + (int32_t)((tile_size.height + 10) *row)},
        .size = {tile_size.width, tile_size.height},
        .horizontal_align = Leading,
        .vertical_align = Top,
        .background_color = bg_color+0x111111,
        .foreground_color = 0
        }), {
        
        if (index < chunk_array_count(entries)){
            launch_entry *entry = chunk_array_get(entries, index);
            label(&ctx, (text_ui_config){
                .slice = entry->info.name.data ? slice_from_literal(entry->info.name.data) : entry->name,
                .font_size = 3,
            }, (common_ui_config){
                .point = RELATIVE(0,0),
                .size = inner_rect.size,
                .horizontal_align = HorizontalCenter,
                .vertical_align = VerticalCenter,
                .background_color = 0,
                .foreground_color = 0xFFFFFFFF,
            });
            string_slice subtext;
            if (entry->info.version.data) subtext = slice_from_literal(entry->info.version.data);
            else if (entry->info.author.data) subtext = slice_from_literal(entry->info.author.data);
            else subtext = entry->ext;
            label(&ctx, (text_ui_config){
                .slice = subtext,
                .font_size = 1,
            }, (common_ui_config){
                .point = RELATIVE(5, 5),
                .size = { 1, 1 },
                .horizontal_align = Leading,
                .vertical_align = Top,
                .background_color = 0,
                .foreground_color = 0xFFFFFFFF,
            });
        }
    });
}

int main(int argc, char* argv[]){
    load_entries();
    selected.x = MAX_COLS/2;
    selected.y = MAX_ROWS/2;
    while (1)
    {
        draw_desktop();
        msleep(process_active ? 25 : 40);
    }
}