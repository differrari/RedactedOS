#include "kconsole.hpp"
#include "graph/graphics.h"

KernelConsole::KernelConsole()
    : cursor_x(0), cursor_y(0)
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
}

void KernelConsole::put_char(char c) {
    if (!check_ready())
        return;

    if (c == '\n') {
        newline();
        return;
    }

    if (cursor_x >= columns) {
        newline();
    }

    buffer[cursor_y][cursor_x] = c;
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
    for (unsigned int y = 1; y < rows; y++) {
        for (unsigned int x = 0; x < columns; x++) {
            buffer[y-1][x] = buffer[y][x];
            gpu_draw_char({x * char_width, (y-1) * char_height}, buffer[y][x], 0xFFFFFFFF);
        }
    }

    for (unsigned int x = 0; x < columns; x++) {
        buffer[rows-1][x] = ' ';
        gpu_draw_char({x * char_width, (rows-1) * char_height}, ' ', 0xFFFFFFFF);
    }
}

void KernelConsole::clear() {
    gpu_clear(0x0);
}