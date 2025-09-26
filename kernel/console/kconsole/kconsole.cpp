#include "kconsole.hpp"
#include "std/memory.h"
#include "syscalls/syscalls.h"

#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0

KernelConsole::KernelConsole() : cursor_x(0), cursor_y(0), is_initialized(false){

}

void KernelConsole::initialize(){
    is_initialized = true;
    dctx = get_ctx();
    resize();
    clear();
    default_text_color = COLOR_WHITE;
    text_color = default_text_color;
}

bool KernelConsole::check_ready(){
    if (!screen_ready()) return false;
    if (!is_initialized){
        initialize();
    }
    return true;
}

void KernelConsole::resize(){
    gpu_size screen_size = {dctx->width,dctx->height};
    columns = screen_size.width / char_width;
    rows = screen_size.height / line_height;

    if (row_data) free(row_data, buffer_data_size);
    buffer_data_size = rows * columns;
    row_data = (char*)malloc(buffer_data_size);
    if (!row_data){
        rows = columns = 0;
        return;
    }

    scroll_row_offset = 0;
}

void KernelConsole::put_char(char c){
    if (!check_ready()) return;
    if (c == '\n'){
        newline(); 
        flush(dctx);
        return;
    }
    if (cursor_x >= columns) newline();

    uint32_t row_index = (scroll_row_offset + cursor_y) % rows;
    char* line = row_data + row_index * columns;
    line[cursor_x] = c;
    fb_draw_char(dctx, cursor_x * char_width, (cursor_y * line_height)+(line_height/2), c, 1, text_color);
    cursor_x++;
}

//TODO: generalize this function to handle movement in general
void KernelConsole::delete_last_char(){
    if (cursor_x > 0){
        cursor_x--;
    } else if (cursor_y > 0){
        cursor_y--;
        cursor_x = 0;
        char* line = row_data + cursor_y * columns;
        while (*line){
            line++;
            cursor_x++;
        } 
        draw_cursor();
        flush(dctx);
        return;
    } else return;
    
    char* line = row_data + cursor_y * columns;
    line[cursor_x] = 0;
    fb_fill_rect(dctx, cursor_x*char_width, cursor_y * line_height, char_width, line_height, COLOR_BLACK);
    draw_cursor();
    flush(dctx);
}

void KernelConsole::draw_cursor(){
    if (last_drawn_cursor_x >= 0 && last_drawn_cursor_y >= 0){
        fb_fill_rect(dctx, last_drawn_cursor_x*char_width, last_drawn_cursor_y * line_height, char_width, line_height, COLOR_BLACK);
        char *line = row_data + (last_drawn_cursor_y * columns);
        fb_draw_char(dctx, last_drawn_cursor_x * char_width, (last_drawn_cursor_y * line_height)+(line_height/2), line[last_drawn_cursor_x], 1, text_color);
    }
    fb_fill_rect(dctx, cursor_x*char_width, cursor_y * line_height, char_width, line_height, COLOR_WHITE);
    last_drawn_cursor_x = cursor_x;
    last_drawn_cursor_y = cursor_y;
}

void KernelConsole::put_string(const char* str){
    if (!check_ready()) return;
    for (uint32_t i = 0; str[i]; i++){
        char c = str[i];
        put_char(c);
    } 
    draw_cursor();
    flush(dctx);
}

void KernelConsole::newline(){
    if (!check_ready()) return;
    uint32_t row_index = (scroll_row_offset + cursor_y) % rows;
    char* line = row_data + row_index * columns;
    for (uint32_t x = cursor_x; x < columns; x++) line[x] = 0;
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= rows - 1){
        scroll();
        cursor_y = rows - 1;
    }
}

void KernelConsole::scroll(){
    if (!check_ready()) return;
    scroll_row_offset = (scroll_row_offset + 1) % rows;
    uint32_t row_index = (scroll_row_offset + cursor_y) % rows;
    char* line = row_data + row_index * columns;
    for (uint32_t x = 0; x < columns; x++) line[x] = 0;
    redraw();
}

void KernelConsole::refresh(){
    resize();
    clear();
    redraw();
    flush(dctx);
}

void KernelConsole::redraw(){
    screen_clear();
    for (uint32_t y = 0; y < rows; y++){
        uint32_t row_index = (scroll_row_offset + y) % rows;
        char* line = row_data + row_index * columns;
        for (uint32_t x = 0; x < columns; x++){
            fb_draw_char(dctx, x * char_width, (y * line_height)+(line_height/2), line[x], 1, text_color);
        }
    }
    draw_cursor();
}

void KernelConsole::screen_clear(){
    fb_clear(dctx, COLOR_BLACK);
    last_drawn_cursor_x = -1;
    last_drawn_cursor_y = -1;
}

void KernelConsole::clear(){
    screen_clear();
    memset(row_data, 0, buffer_data_size);
    cursor_x = cursor_y = 0;
}

const char* KernelConsole::get_current_line(){
    uint32_t row_index = (scroll_row_offset + cursor_y) % rows;
    return row_data + row_index * columns;
}

void KernelConsole::set_text_color(uint32_t hex){
    text_color = hex | 0xFF000000;
}

#include "graph/graphics.h"

draw_ctx* KernelConsole::get_ctx(){
    return gpu_get_ctx();
}

void KernelConsole::flush(draw_ctx *ctx){
    gpu_flush();
}

bool KernelConsole::screen_ready(){
    return gpu_ready();
}
