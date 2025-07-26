#include "desktop.hpp"
#include "console/kio.h"
#include "graph/graphics.h"
#include "theme/theme.h"
#include "input/input_dispatch.h"
#include "memory/kalloc.h"
#include "std/string.h"
#include "filesystem/disk.h"
#include "process/loading/elf_file.h"

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
    string_list *list = list_directory_contents("/redos/user/");
    if (list){
        char* reader = (char*)list->array;
        for (uint32_t i = 0; i < list->count; i++){
            char *file = reader;
            string fullpath = string_format("/redos/user/%s",(uintptr_t)file);
            string name = string_ca_max(file,find_extension(file));
            string ext = string_l(file + find_extension(file));
            if (strcmp(ext.data,".elf", true) == 0){
                kprintf("Extension %s matches .elf", (uintptr_t)ext.data);
                add_entry(name.data, ext.data, fullpath.data);
            }
            while (*reader) reader++;
            reader++;
        }
    }
    
    single_label = new Label();
    extension_label = new Label();
}

void Desktop::draw_desktop(){
    if (!await_gpu()) return;
    if (active_proc != nullptr && active_proc->state != process_t::process_state::STOPPED) return;
    if (process_active){
        active_proc = nullptr;
        sys_focus_current();
        rendered_full = false;
    }
    keypress kp;
    gpu_point old_selected = selected;
    while (sys_read_input_current(&kp)){
        for (int i = 0; i < 6; i++){
            char key = kp.keys[i];
            if (key == KEY_ENTER || key == KEY_KEYPAD_ENTER){
                activate_current();
                return;
            }
            if (key == KEY_ARROW_RIGHT){
                selected.x = (selected.x + 1) % MAX_COLS;
            }
            if (key == KEY_ARROW_LEFT){
                selected.x = (selected.x - 1 + MAX_COLS) % MAX_COLS;
            } 
            if (key == KEY_ARROW_DOWN){
                selected.y = (selected.y + 1) % MAX_ROWS;
            } 
            if (key == KEY_ARROW_UP){
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
        gpu_flush();
    }
}

void Desktop::draw_full(){
    gpu_clear(BG_COLOR);
    for (uint32_t column = 0; column < MAX_COLS; column++){
        for (uint32_t row = 0; row < MAX_ROWS; row++){
            draw_tile(column, row);
        }
    }
    gpu_flush();
}

bool Desktop::await_gpu(){
    if (!gpu_ready())
        return false;
    if (!ready){
        disable_visual();
        sys_focus_current();
        gpu_size screen_size = gpu_get_screen_size();
        tile_size = {screen_size.width/MAX_COLS - 20, screen_size.height/(MAX_ROWS+1) - 20};
        ready = true;
    }
    return ready;
}

void Desktop::activate_current(){
    uint32_t index = (selected.y * MAX_COLS) + selected.x;

    if (index < entries.size()){
        if (strcmp(".elf",entries[index].ext, true) != 0){
            kprintf("Wrong executable format. Must be .elf");
            return;
        }
        kprintf("File path %s",(uintptr_t)entries[index].path);
        void *file = read_file(entries[index].path);
        active_proc = load_elf_file(entries[index].name, file);
        if (!active_proc){
            kprintf("Failed to read ELF file");
            return;
        }
        process_active = true;
        sys_set_focus(active_proc->id);
    }
    
}

void Desktop::draw_tile(uint32_t column, uint32_t row){
    bool sel = selected.x == column && selected.y == row;
    uint32_t index = (row * MAX_COLS) + column;
    
    int border = 4;
    
    if (sel)
        gpu_fill_rect((gpu_rect){10 + ((tile_size.width + 10)*column), 50 + ((tile_size.height + 10) *row), tile_size.width, tile_size.height}, BG_COLOR+0x333333);
    gpu_rect inner_rect = (gpu_rect){10 + ((tile_size.width + 10)*column)+ (sel ? border : 0), 50 + ((tile_size.height + 10) *row) + (sel ? border : 0), tile_size.width - (sel ? border * 2 : 0), tile_size.height - (sel ? border * 2 : 0)};
    gpu_fill_rect(inner_rect, BG_COLOR+0x111111);
    if (index < entries.size()){
        string namestr = string_l(entries[index].name);
        string extstr = string_l(entries[index].ext);

        single_label->set_text(namestr);
        single_label->set_bg_color(BG_COLOR+0x111111);
        single_label->set_text_color(0xFFFFFFFF);
        single_label->set_font_size(3);
        single_label->rect = inner_rect;
        single_label->set_alignment(HorizontalAlignment::HorizontalCenter, VerticalAlignment::VerticalCenter);
        single_label->render();
        
        extension_label->set_text(extstr);
        extension_label->set_bg_color(BG_COLOR+0x111111);
        extension_label->set_text_color(0xFFFFFFFF);
        extension_label->set_font_size(1);
        extension_label->rect = (gpu_rect){10 + ((tile_size.width + 10)*column)+ (sel ? border*2 : border), 50 + ((tile_size.height + 10) *row) + (sel ? border*2 : border), 1, 1};
        extension_label->set_alignment(HorizontalAlignment::Leading, VerticalAlignment::Top);
        extension_label->render();
        free(namestr.data, namestr.mem_length);
        free(extstr.data, extstr.mem_length);
    }
}