#include "desktop.hpp"
#include "console/kio.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "input/input_dispatch.h"
#include "memory/talloc.h"
#include "std/string.h"
#include "filesystem/filesystem.h"
#include "process/loading/elf_file.h"
#include "ui/uno/uno.h"
#include "graph/tres.h"

#define MAX_COLS 3
#define MAX_ROWS 3

void Desktop::add_entry(char* name, char *ext, char* path){
    entries.add({
        .name = name,
        .ext = ext,
        .path = path,
    });
}

uint16_t Desktop::find_extension(char *path){
    uint16_t count = 0;
    while (*path && *path != '.'){ path++; count++; }
    return path ? count : 0;
}

Desktop::Desktop() {
    entries = Array<LaunchEntry>(9);
    sizedptr list_ptr = list_directory_contents("/boot/redos/user/");
    string_list *list = (string_list*)list_ptr.ptr;
    if (list && list_ptr.size){
        char* reader = (char*)list->array;
        for (uint32_t i = 0; i < list->count; i++){
            char *file = reader;
            string fullpath = string_format("/boot/redos/user/%s",(uintptr_t)file);
            string name = string_from_literal_length(file,find_extension(file));
            string ext = string_from_literal(file + find_extension(file));
            if (strcmp(ext.data,".elf", true) == 0){
                add_entry(name.data, ext.data, fullpath.data);
            }
            while (*reader) reader++;
            reader++;
        }
        //TODO: The list of strings needs to be freed, but this class is not its owner
    }
}

void Desktop::draw_desktop(){
    if (!await_gpu()) return;
    if (active_proc != nullptr && active_proc->state != process_t::process_state::STOPPED) return;
    if (process_active){
        active_proc = nullptr;
        sys_focus_current();
        rendered_full = false;
        process_active = false;
    }
    keypress kp;
    gpu_point old_selected = selected;
    while (sys_read_input_current(&kp)){
        for (int i = 0; i < 6; i++){
            char key = kp.keys[i];
            if (key == KEY_ENTER || key == KEY_KPENTER){
                activate_current();
                return;
            }
            if (key == KEY_RIGHT){
                selected.x = (selected.x + 1) % MAX_COLS;
            }
            if (key == KEY_LEFT){
                selected.x = (selected.x - 1 + MAX_COLS) % MAX_COLS;
            } 
            if (key == KEY_DOWN){
                selected.y = (selected.y + 1) % MAX_ROWS;
            } 
            if (key == KEY_UP){
                selected.y = (selected.y - 1 + MAX_ROWS) % MAX_ROWS;
            } 
        }
    }
    if (!rendered_full){
        rendered_full = true;
        draw_full();
    } else if (old_selected.x != selected.x || old_selected.y != selected.y){
        draw_tile(old_selected.x, old_selected.y);
        draw_tile(selected.x, selected.y);
        commit_frame(&ctx);
    }
}

void Desktop::draw_full(){
    fb_clear(&ctx, BG_COLOR+0x050505);
    for (uint32_t column = 0; column < MAX_COLS; column++){
        for (uint32_t row = 0; row < MAX_ROWS; row++){
            draw_tile(column, row);
        }
    }
    commit_frame(&ctx);
}

bool Desktop::await_gpu(){
    if (!gpu_ready())
        return false;
    if (!ready){
        get_window_ctx(&ctx);
        sys_focus_current();
        gpu_size screen_size = {ctx.width, ctx.height};
        tile_size = {screen_size.width/MAX_COLS - 20, screen_size.height/(MAX_ROWS+1) - 20};
        ready = true;
    }
    return ready;
}

void Desktop::activate_current(){
    uint32_t index = (selected.y * MAX_COLS) + selected.x;

    if (index < entries.size()){
        if (strcmp(".elf",entries[index].ext, true) != 0){
            kprintf("[LAUNCHER] Wrong executable format. Must be .elf");
            return;
        }
        kprintf("[LAUNCHER] File path %s",(uintptr_t)entries[index].path);
        file fd = {};
        FS_RESULT rest = open_file(entries[index].path, &fd);
        kprintf(" Open file %i", rest);
        if (rest != FS_RESULT_SUCCESS)
        {
            kprintf("[LAUNCHER] Failed to open file");
            return;
        }
        char *file = (char*)malloc(fd.size);
        kprintf("[LAUNCHER] opened file");
        if (read_file(&fd, file, fd.size) != fd.size){
            kprintf("[LAUNCHER] Failed to read full elf file");
            return;
        } 
        kprintf("[LAUNCHER] read file %x",fd.size);
        active_proc = load_elf_file(entries[index].name, file,fd.size);
        if (!active_proc){
            kprintf("[LAUNCHER] Failed to load ELF file");
            return;
        }
        active_proc->priority = PROC_PRIORITY_FULL;
        kprintf("[LAUNCHER] process launched");
        fb_clear(&ctx, 0);
        commit_draw_ctx(&ctx);
        process_active = true;
        sys_set_focus(active_proc->id);
    }
    
}

void Desktop::draw_tile(uint32_t column, uint32_t row){
    bool sel = selected.x == column && selected.y == row;
    uint32_t index = (row * MAX_COLS) + column;
    
    int border = 4;

    gpu_rect inner_rect = (gpu_rect){{10 + ((tile_size.width + 10)*column)+ (sel ? border : 0), 50 + ((tile_size.height + 10) *row) + (sel ? border : 0)}, {tile_size.width - (sel ? border * 2 : 0), tile_size.height - (sel ? border * 2 : 0)}};

    DRAW(
        rectangle(&ctx, {
        .border_size = (uint8_t)(sel ? 4 : 0),
        .border_color = BG_COLOR+0x333333,
        }, (common_ui_config){
        .point = {10 + ((tile_size.width + 10)*column), 50 + ((tile_size.height + 10) *row)},
        .size = {tile_size.width, tile_size.height},
        .horizontal_align = Leading,
        .vertical_align = Top,
        .background_color = BG_COLOR+0x111111,
        .foreground_color = 0
        }), {
        
        if (index < entries.size()){
            
            label(&ctx, (text_ui_config){
                .text = entries[index].name,
                .font_size = 3,
            }, (common_ui_config){
                .point = RELATIVE(0,0),
                .size = inner_rect.size,
                .horizontal_align = HorizontalCenter,
                .vertical_align = VerticalCenter,
                .background_color = 0,
                .foreground_color = COLOR_WHITE,
            });

            label(&ctx, (text_ui_config){
                .text = entries[index].ext,
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