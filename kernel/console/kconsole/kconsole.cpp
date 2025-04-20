#include "kconsole.hpp"
#include "mmio.h"
#include "graph/graphics.h"

KernelConsole::KernelConsole()
    : cursor_x(0), cursor_y(0), scroll_row_offset(0)
{
    resize();
    clear();
}

bool KernelConsole::check_ready(){
    if (!gpu_ready()) return false;
    if (!is_initialized){
        resize();
        clear();
        is_initialized = true;
    }
    return true;
}

void KernelConsole::resize() {
    size screen = gpu_get_screen_size();
    columns = screen.width / char_width;
    rows = screen.height / char_height;
    buffer = (char**)alloc(rows * sizeof(char*));
    for (unsigned int i = 0; i < rows; i++) {
        buffer[i] = (char*)alloc(columns * sizeof(char));
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
    gpu_draw_char({cursor_x * char_width, cursor_y * char_height}, c, 0xFFFFFFFF);
    cursor_x++;
}

void KernelConsole::put_string(const char *str) {
    if (!check_ready())
        return;
    for (uint32_t i = 0; str[i] != 0; i++) {
        put_char(str[i]);
    }
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
            gpu_draw_char({x * char_width, y * char_height}, c, 0xFFFFFFFF);
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
}