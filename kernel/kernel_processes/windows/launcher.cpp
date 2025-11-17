#include "launcher.hpp"
#include "console/kio.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "input/input_dispatch.h"
#include "std/string.h"
#include "filesystem/filesystem.h"
#include "process/loading/elf_file.h"
#include "ui/uno/uno.h"
#include "graph/tres.h"
#include "process/scheduler.h"
#include "exceptions/irq.h"

#define MAX_COLS 3
#define MAX_ROWS 3

void Launcher::add_entry(string name, string ext, string path, package_info info){
    entries.add({
        .name = name,
        .ext = ext,
        .path = path,
        .info = info,
    });
}

uint16_t Launcher::find_extension(char *path){
    uint16_t count = 0;
    while (*path && *path != '.'){ path++; count++; }
    return path ? count : 0;
}

Launcher::Launcher() {
    entries = Array<LaunchEntry>(9);
    load_entries();
}

package_info Launcher::get_pkg_info(char* info_path){
    file fd = {};
    FS_RESULT res = fopen(info_path, &fd);
    if (res != FS_RESULT_SUCCESS) return (package_info){};

    char *info = (char*)malloc(fd.size);
    fread(&fd, info, fd.size);

    return parse_package_info(info, fd.size);
}

void Launcher::load_entries(){
    for (uint32_t i = 0; i < entries.size(); i++){
        string_free(entries[i].name);
        string_free(entries[i].ext);
        string_free(entries[i].path);
        string_free(entries[i].info.name);
        string_free(entries[i].info.version);
        string_free(entries[i].info.author);
    }
    entries.empty();
    size_t listsize = 0x1000;
    void *listptr = malloc(listsize);
    if (!list_directory_contents("/boot/redos/user/", listptr, listsize, 0)){
        kprintf("Failed to read contents of directory");
        return;
    }
    string_list *list = (string_list*)listptr;
    if (list){
        char* reader = (char*)list->array;
        for (uint32_t i = 0; i < list->count; i++){
            char *file = reader;
            if (*file){
                string fullpath = string_format("/boot/redos/user/%s",(uintptr_t)file);
                string name = string_from_literal_length(file,find_extension(file));
                string ext = string_from_literal(file + find_extension(file));
                if (strcmp(ext.data,".red", true) == 0){
                    string pkg_info = string_concat(fullpath, string_from_literal("/package.info"));
                    add_entry(name, ext, fullpath, get_pkg_info(pkg_info.data));
                    free(pkg_info.data, pkg_info.mem_length);
                }
                while (*reader) reader++;
                reader++;
            }
        }
    }
    free(listptr, listsize);
}

void Launcher::draw_desktop(){
    if (!await_gpu()) return;
    if (active_proc != nullptr && active_proc->state != process_t::process_state::STOPPED) return;
    if (process_active){
        active_proc = nullptr;
        sys_focus_current();
        load_entries();
        request_draw_ctx(&ctx);
        if (ctx.width < 512 || ctx.height < 256){
            resize_draw_ctx(&ctx, max(512,ctx.width), max(256, ctx.height));
        }
        gpu_size screen_size = {ctx.width, ctx.height};
        tile_size = { screen_size.width/MAX_COLS - 20, screen_size.height/(MAX_ROWS+1) - 20 };
        rendered_full = false;
        process_active = false;
    }
    gpu_point old_selected = selected;
    kbd_event event;
    while (sys_read_event_current(&event)){
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
    } else if (old_selected.x != selected.x || old_selected.y != selected.y){
        draw_tile(old_selected.x, old_selected.y);
        draw_tile(selected.x, selected.y);
    }
    ctx.full_redraw = true;
    commit_draw_ctx(&ctx);
}

void Launcher::draw_full(){
    if (!await_gpu()) return;
    fb_clear(&ctx, system_theme.bg_color+0x050505);
    for (uint32_t column = 0; column < MAX_COLS; column++){
        for (uint32_t row = 0; row < MAX_ROWS; row++){
            draw_tile(column, row);
        }
    }
}

bool Launcher::await_gpu(){
    if (!gpu_ready())
        return false;
    if (!ready){
        request_draw_ctx(&ctx);
        sys_focus_current();
        gpu_size screen_size = {ctx.width, ctx.height};
        tile_size = {screen_size.width/MAX_COLS - 20, screen_size.height/(MAX_ROWS+1) - 20};
        ready = true;
    }
    return ready;
}

void Launcher::activate_current(){
    uint32_t index = (selected.y * MAX_COLS) + selected.x;

    if (index < entries.size()){
        if (strcmp(".red",entries[index].ext.data, true) != 0){
            kprintf("[LAUNCHER] Wrong format. Must be a .red package");
            return;
        }
        string s = string_format("%s/%s.elf",entries[index].path.data, entries[index].name.data);
        file fd = {};
        FS_RESULT rest = open_file(s.data, &fd);
        free(s.data, s.mem_length);
        kprintf(" Open file %i", rest);
        if (rest != FS_RESULT_SUCCESS)
        {
            kprintf("[LAUNCHER] Failed to open file %i",rest);
            return;
        }
        char *file = (char*)malloc(fd.size);
        kprintf("[LAUNCHER] opened file");
        size_t read_size = read_file(&fd, file, fd.size);
        close_file(&fd);
        if (read_size != fd.size){
            kprintf("[LAUNCHER] Failed to read full elf file");
            rendered_full = false;
            return;
        } 
        fb_clear(&ctx, 0);
        commit_draw_ctx(&ctx);
        kprintf("[LAUNCHER] read file %x",fd.size);
        disable_interrupt();
        active_proc = load_elf_file(entries[index].name.data,entries[index].path.data, file,fd.size);
        if (!active_proc){
            kprintf("[LAUNCHER] Failed to load ELF file");
            rendered_full = false;
            return;
        }
        active_proc->win_id = get_current_proc()->win_id;
        active_proc->priority = PROC_PRIORITY_FULL;
        kprintf("[LAUNCHER] process launched");
        enable_interrupt();
        process_active = true;
        sys_set_focus(active_proc->id);
        active_proc->state = process_t::process_state::READY;
    }
    
}

void Launcher::draw_tile(uint32_t column, uint32_t row){
    bool sel = selected.x == column && selected.y == row;
    uint32_t index = (row * MAX_COLS) + column;
    
    int border = 4;

    gpu_rect inner_rect = (gpu_rect){{10 + ((tile_size.width + 10)*column)+ (sel ? border : 0), 50 + ((tile_size.height + 10) *row) + (sel ? border : 0)}, {tile_size.width - (sel ? border * 2 : 0), tile_size.height - (sel ? border * 2 : 0)}};

    DRAW(
        rectangle(&ctx, {
        .border_size = (uint8_t)(sel ? 4 : 0),
        .border_color = system_theme.bg_color+0x333333,
        }, (common_ui_config){
        .point = {10 + ((tile_size.width + 10)*column), 50 + ((tile_size.height + 10) *row)},
        .size = {tile_size.width, tile_size.height},
        .horizontal_align = Leading,
        .vertical_align = Top,
        .background_color = system_theme.bg_color+0x111111,
        .foreground_color = 0
        }), {
        
        if (index < entries.size()){
            
            label(&ctx, (text_ui_config){
                .text = entries[index].info.name.data ? entries[index].info.name.data : entries[index].name.data,
                .font_size = 3,
            }, (common_ui_config){
                .point = RELATIVE(0,0),
                .size = inner_rect.size,
                .horizontal_align = HorizontalCenter,
                .vertical_align = VerticalCenter,
                .background_color = 0,
                .foreground_color = COLOR_WHITE,
            });
            char *subtext;
            if (entries[index].info.version.data) subtext = entries[index].info.version.data;
            else if (entries[index].info.author.data) subtext = entries[index].info.author.data;
            else subtext = entries[index].ext.data;
            label(&ctx, (text_ui_config){
                .text = subtext,
                .font_size = 1,
            }, (common_ui_config){
                .point = RELATIVE(5, 5),
                .size = { 1, 1 },
                .horizontal_align = Leading,
                .vertical_align = Top,
                .background_color = 0,
                .foreground_color = COLOR_WHITE,
            });
        }
    });

    
}

extern "C" int manage_window(int argc, char* argv[]){
    Launcher *desktop = new Launcher();
    while (1)
    {
        desktop->draw_desktop();
    }
}

extern "C" process_t* launch_launcher(){
    process_t *p = create_kernel_process("winmanager",manage_window, 0, 0);
    p->priority = PROC_PRIORITY_FULL;
    return p;
}