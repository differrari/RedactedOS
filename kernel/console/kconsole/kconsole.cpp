#include "kconsole.hpp"
#include "memory/kalloc.h"
#include "graph/graphics.h"
#include "console/serial/uart.h"

KernelConsole::KernelConsole()
    : cursor_x(0), cursor_y(0), scroll_row_offset(0)
{
    resize();
    clear();
}

bool KernelConsole::check_ready(){
    if (!gpu_ready()) return false;
    if (!is_initialized){
        is_initialized = true;
        resize();
        clear();
    }
    return true;
}

void KernelConsole::resize() {
    gpu_size screen = gpu_get_screen_size();
    uart_puthex(screen.width);
    columns = screen.width / char_width;
    rows = screen.height / char_height;

    if (buffer_header_size > 0)
        temp_free(buffer,buffer_header_size);
    if (buffer_data_size > 0)
        temp_free(row_data,buffer_data_size);

    buffer_header_size = rows * sizeof(char*);
    buffer = (char**)talloc(rows * sizeof(char*));
    buffer_data_size = rows * columns * sizeof(char);
    row_data = (char*)talloc(buffer_data_size);

    for (unsigned int i = 0; i < rows; i++) {
        buffer[i] = row_data + (i * columns);
    }
}

void KernelConsole::put_char(char c) {
    if (!check_ready())
        return;

    if (c == '\n') {
        newline();
        return;
    }

    if (cursor_x >= columns)
        newline();

    buffer[(scroll_row_offset + cursor_y) % rows][cursor_x] = c;
    gpu_draw_char({cursor_x * char_width, cursor_y * char_height}, c, 1, 0xFFFFFFFF);
    cursor_x++;
}

void KernelConsole::put_string(const char *str) {
    if (!check_ready())
        return;
    for (uint32_t i = 0; str[i] != 0; i++) {
        put_char(str[i]);
    }
    gpu_flush();
}

void KernelConsole::put_hex(uint64_t value) {
    if (!check_ready())
        return;
    put_char('0');
    put_char('x');
    bool started = false;
    for (uint32_t i = 60;; i -= 4) {
        uint8_t nibble = (value >> i) & 0xF;
        char curr_char = nibble < 10 ? '0' + nibble : 'A' + (nibble - 10);
        if (started || curr_char != '0' || i == 0) {
            started = true;
            put_char(curr_char);
        }
        if (i == 0) break;
    }

    gpu_flush();
}

void KernelConsole::newline() {
    if (!check_ready())
        return;
    for (unsigned x = cursor_x; x < columns; x++){
        buffer[(scroll_row_offset + cursor_y) % rows][x] = 0;
    }
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= rows) {
        scroll();
        cursor_y = rows - 1;
    }
}

void KernelConsole::scroll() {
    if (!check_ready())
        return;

    scroll_row_offset = (scroll_row_offset + 1) % rows;

    for (unsigned int x = 0; x < columns; x++) {
        buffer[(scroll_row_offset + rows - 1) % rows][x] = 0;
    }

    redraw();
}

void KernelConsole::redraw(){
    screen_clear();
    for (unsigned int y = 0; y < rows; y++) {
        for (unsigned int x = 0; x < columns; x++) {
            char c = buffer[(scroll_row_offset + y) % rows][x];
            gpu_draw_char({x * char_width, y * char_height}, c, 1, 0xFFFFFFFF);
        }
    }
}

void KernelConsole::screen_clear(){
    gpu_clear(0x0);
}

void KernelConsole::clear() {
    screen_clear();
    for (unsigned int y = 0; y < rows; y++) {
        for (unsigned int x = 0; x < columns; x++) {
            buffer[y][x] = 0;
        }
    }
    cursor_x = 0;
    cursor_y = 0;
    scroll_row_offset = 0;
    gpu_flush();
}