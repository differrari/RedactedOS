#pragma once

#include "types.h"
#include "data_struct/ring_buffer.hpp"
#include "ui/draw/draw.h"

#define COLOR_WHITE 0xFFFFFFFF
#define COLOR_BLACK 0

class Console{
public:
    Console();

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
    const char* get_current_line();

    virtual draw_ctx* get_ctx() = 0;
    virtual void flush(draw_ctx *ctx) = 0;
    virtual bool screen_ready() = 0;

    void set_text_color(uint32_t color);

    uint32_t cursor_x, cursor_y;
    int32_t last_drawn_cursor_x, last_drawn_cursor_y;
    uint32_t columns, rows;
    bool is_initialized;
    
    static constexpr uint32_t max_rows=128;

    uint32_t default_bg_color = COLOR_BLACK;
    uint32_t default_text_color = COLOR_WHITE;
    uint32_t bg_color = default_bg_color;
    uint32_t text_color = default_text_color;
    
    int32_t scroll_row_offset = 0;
    char* row_data;
    uint32_t gap_start, gap_end;
    uint32_t buffer_data_size;

    draw_ctx *dctx;

    bool active = true;

    uint8_t char_scale = 1;
};

