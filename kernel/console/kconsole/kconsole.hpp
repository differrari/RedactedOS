#pragma once

#include "types.h"
#include "data_struct/ring_buffer.hpp"
#include "graph/graphics.h"
#include "memory/kalloc.h"
#include "ui/draw/draw.h"

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
    void delete_last_char();
    
protected:
    bool check_ready();
    void screen_clear();
    void redraw();
    void draw_cursor();

    uint32_t cursor_x, cursor_y;
    int32_t last_drawn_cursor_x, last_drawn_cursor_y;
    uint32_t columns, rows;
    bool is_initialized;

    static constexpr uint32_t char_width=CHAR_SIZE;
    static constexpr uint32_t line_height=CHAR_SIZE*2;
    static constexpr uint32_t max_rows=128;

    RingBuffer<uint32_t, max_rows> row_ring;
    char* row_data;
    uint32_t gap_start, gap_end;
    uint32_t buffer_data_size;

    void *mem_page;
    bool active = true;
};

extern KernelConsole kconsole;
