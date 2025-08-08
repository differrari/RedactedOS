#include "kconsole.hpp"
#include "console/serial/uart.h"
#include "memory/page_allocator.h"
#include "filesystem/filesystem.h"
#include "theme/theme.h"

KernelConsole::KernelConsole() : cursor_x(0), cursor_y(0), is_initialized(false){
    initialize();
}

void KernelConsole::initialize(){
    is_initialized = true;
    mem_page = palloc(PAGE_SIZE, true, true, false);
    resize();
    clear();
}

bool KernelConsole::check_ready(){
    if (!gpu_ready()) return false;
    if (!is_initialized){
        initialize();
    }
    return true;
}

void KernelConsole::resize(){
    gpu_size screen_size = gpu_get_screen_size();
    columns = screen_size.width / char_width;
    rows = screen_size.height / line_height;

    if (row_data) kfree(row_data, buffer_data_size);
    buffer_data_size = rows * columns;
    row_data = (char*)kalloc(mem_page, buffer_data_size, ALIGN_16B, true, true);
    if (!row_data){
        rows = columns = 0;
        row_ring.clear();
        return;
    }

    row_ring.clear();
    for (uint32_t i = 0; i < rows; i++) row_ring.push(i);
}

void KernelConsole::put_char(char c){
    if (!check_ready()) return;
    if (c == '\n'){
        newline(); 
        gpu_flush();
        return;
    }
    if (cursor_x >= columns) newline();

    uint32_t row_index = row_ring.peek();
    char* line = row_data + row_index * columns;
    line[cursor_x] = c;
    gpu_draw_char({cursor_x * char_width, (cursor_y * line_height)+(line_height/2)}, c, 1, COLOR_WHITE);
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
            uart_putc(*line);
            line++;
            cursor_x++;
        } 
        draw_cursor();
        gpu_flush();
        return;
    } else return;
    
    char* line = row_data + cursor_y * columns;
    line[cursor_x] = 0;
    gpu_fill_rect({{cursor_x*char_width, cursor_y * line_height}, {char_width, line_height}}, COLOR_BLACK);
    draw_cursor();
    gpu_flush();
}

void KernelConsole::draw_cursor(){
    if (last_drawn_cursor_x >= 0 && last_drawn_cursor_y >= 0){
        gpu_fill_rect({{last_drawn_cursor_x*char_width, last_drawn_cursor_y * line_height}, {char_width, line_height}}, COLOR_BLACK);
        char *line = row_data + (last_drawn_cursor_y * columns);
        uart_putc(line[last_drawn_cursor_x]);
        gpu_draw_char({last_drawn_cursor_x * char_width, (last_drawn_cursor_y * line_height)+(line_height/2)}, line[last_drawn_cursor_x], 1, COLOR_WHITE);
    }
    gpu_fill_rect({{cursor_x*char_width, cursor_y * line_height}, {char_width, line_height}}, COLOR_WHITE);
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
    gpu_flush();
}

void KernelConsole::newline(){
    if (!check_ready()) return;
    uint32_t row_index;
    if (row_ring.pop(row_index)){
        row_ring.push(row_index);
        char* line = row_data + row_index * columns;
        for (uint32_t x = cursor_x; x < columns; x++) line[x] = 0;
    }
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= rows - 1){
        scroll();
        cursor_y = rows - 1;
    }
}

void KernelConsole::scroll(){
    if (!check_ready()) return;
    if (uint32_t row_index = row_ring.peek()){
        char* line = row_data + row_index * columns;
        for (uint32_t x = 0; x < columns; x++) line[x] = 0;
    }
    redraw();
}

void KernelConsole::refresh(){
    resize();
    clear();
    redraw();
    gpu_flush();
}

void KernelConsole::redraw(){
    screen_clear();
    for (uint32_t y = 0; y < rows; y++){
        uint32_t row_index;
        if (row_ring.pop(row_index)){
            row_ring.push(row_index);
            char* line = row_data + row_index * columns;
            for (uint32_t x = 0; x < columns; x++){
                gpu_draw_char({x * char_width, (y * line_height)+(line_height/2)}, line[x], 1, COLOR_WHITE);
            }
        }
    }
    draw_cursor();
}

void KernelConsole::screen_clear(){
    gpu_clear(COLOR_BLACK);
    last_drawn_cursor_x = -1;
    last_drawn_cursor_y = -1;
}

void KernelConsole::clear(){
    screen_clear();
    for (uint32_t i = 0; i < rows; i++){
        uint32_t row_index;
        if (row_ring.pop(row_index)){
            char* line = row_data + row_index * columns;
            for (uint32_t x = 0; x < columns; x++) line[x] = 0;
            row_ring.push(row_index);
        }
    }
    cursor_x = cursor_y = 0;
}
