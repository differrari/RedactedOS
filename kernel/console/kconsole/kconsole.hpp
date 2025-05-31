#pragma once

#include "types.h"

class KernelConsole {
public:
    KernelConsole();

    void put_char(char c);
    void put_string(const char *str);
    void put_hex(uint64_t value);
    void newline();
    void scroll();
    void clear();
    void resize();

private:
    bool check_ready();
    void screen_clear();
    void redraw();

    unsigned int cursor_x;
    unsigned int cursor_y;
    unsigned int columns;
    unsigned int rows;
    bool is_initialized = false;
    int scroll_row_offset = 0;
    static constexpr int char_width = 8;
    static constexpr int char_height = 16;
    char** buffer;
    char* row_data;

    uint64_t buffer_header_size;
    uint64_t buffer_data_size;
};
extern KernelConsole kconsole;