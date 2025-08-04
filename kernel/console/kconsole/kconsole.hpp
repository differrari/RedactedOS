#pragma once

#include "types.h"
#include "data_struct/ring_buffer.hpp"
#include "graph/graphics.h"
#include "memory/kalloc.h"

class KernelConsole{
public:
    KernelConsole();

    void initialize();

    void put_char(char c);
    void put_string(const char* str);

    void newline();
    void scroll();
    void clear();
    void resize();

    void refresh();
    
    void set_active(bool active);
    void handle_input();
    
private:
    bool check_ready();
    void screen_clear();
    void redraw();

    uint32_t cursor_x, cursor_y;
    uint32_t columns, rows;
    bool is_initialized;

    static constexpr uint32_t char_width=8;
    static constexpr uint32_t char_height=16;
    static constexpr uint32_t max_rows=128;

    RingBuffer<uint32_t, max_rows> row_ring;
    char* row_data;
    uint32_t buffer_data_size;

    void *mem_page;
    bool active = true;
};

extern KernelConsole kconsole;
