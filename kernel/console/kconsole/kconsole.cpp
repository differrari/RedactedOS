#include "kconsole.hpp"
#include "console/serial/uart.h"
#include "memory/page_allocator.h"
#include "filesystem/filesystem.h"

KernelConsole::KernelConsole() : cursor_x(0), cursor_y(0), is_initialized(false){
    resize();
    clear();
}

bool KernelConsole::check_ready(){
    if (!gpu_ready()) return false;
    if (!is_initialized){
        is_initialized= true;
        mem_page = palloc(PAGE_SIZE, true, true, false);
        resize();
        clear();
    }
    return true;
}

void KernelConsole::resize(){
    gpu_size screen_size = gpu_get_screen_size();
    columns = screen_size.width / char_width;
    rows = screen_size.height / char_height;

    if (row_data) kfree(row_data, buffer_data_size);
    buffer_data_size = rows * columns;
    uart_puts("Data Size ");
    uart_puthex(buffer_data_size);
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
        return;
    }
    if (cursor_x >= columns) newline();

    uint32_t row_index = row_ring.peek();
    char* line = row_data + row_index * columns;
    line[cursor_x] = c;
    gpu_draw_char({cursor_x * char_width, cursor_y * char_height}, c, 1, 0xFFFFFFFF);
    cursor_x++;
}

void KernelConsole::put_string(const char* str){
    if (!check_ready()) return;
    for (uint32_t i = 0; str[i]; i++){
        char c = str[i];
        put_char(c);
        if (c == '\n') gpu_flush();
    } 
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
    if (cursor_y >= rows){
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

void KernelConsole::refresh_input(){
    resize();
    clear();
    void *content = read_file("/dev/console", buffer_data_size);
    row_data = (char*)content;
    redraw();
}

void KernelConsole::redraw(){
    screen_clear();
    for (uint32_t y = 0; y < rows; y++){
        uint32_t row_index;
        if (row_ring.pop(row_index)){
            row_ring.push(row_index);
            char* line = row_data + row_index * columns;
            for (uint32_t x = 0; x < columns; x++){
                gpu_draw_char({x * char_width, y * char_height}, line[x], 1, 0xFFFFFFFF);
            }
        }
    }
}

void KernelConsole::screen_clear(){
    gpu_clear(0x0);
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