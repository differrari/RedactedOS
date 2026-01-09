#include "console.hpp"
#include "std/memory.h"
#include "syscalls/syscalls.h"
#include "math/math.h"

#define char_width (char_scale*CHAR_SIZE)
#define line_height (char_scale*CHAR_SIZE*2)

static inline uint32_t line_len(const char* line, uint32_t max_cols){
    uint32_t n = 0;
    while (n < max_cols && line[n]) n++;
    return n;
}

Console::Console() : cursor_x(0), cursor_y(0), is_initialized(false){

}

void Console::initialize(){
    is_initialized = true;
    dctx = get_ctx();
    resize();
    screen_clear();
}

bool Console::check_ready(){
    if (!screen_ready()) return false;
    if (!is_initialized){
        initialize();
    }
    return true;
}

void Console::resize(){
    if (!dctx){
        rows = columns = 0;
        return;
    }

    gpu_size screen_size = { dctx->width, dctx->height };
    uint32_t new_columns = screen_size.width / char_width;
    uint32_t new_rows = screen_size.height / line_height;

    if (new_rows == 0 || new_columns == 0) { rows = columns = 0; return; }

    char* old_data = row_data;
    uint32_t old_rows = rows;
    uint32_t old_cols = columns;
    uint32_t old_offset = scroll_row_offset;

    uint32_t new_buf_size = new_rows * new_columns;
    char* new_data = (char*)malloc(new_buf_size);
    if (!new_data) return;
    memset(new_data, 0, new_buf_size);

    if (old_data && old_rows && old_cols) {
        uint32_t copy_rows = min(old_rows, new_rows);

        for (uint32_t y = 0; y < copy_rows; y++) {
            const char* src = old_data + (((old_offset + y) % old_rows) * old_cols);
            char* dst = new_data + (y * new_columns);
            uint32_t len = line_len(src, old_cols);
            uint32_t w = min(len, new_columns);
            if (w) memcpy(dst, src, w);
            if (w < new_columns) dst[w] = 0;
        }
    }

    if (old_data) free_sized(old_data, old_rows * old_cols);

    row_data = new_data;
    rows = new_rows;
    columns = new_columns;
    buffer_data_size = new_buf_size;
    scroll_row_offset = 0;

    if ((int32_t)cursor_y < 0) cursor_y = 0;
    if ((int32_t)cursor_x < 0) cursor_x = 0;
    if (cursor_y >= rows) cursor_y = rows ? rows - 1 : 0;
    if (cursor_x >= columns) cursor_x = columns ? columns - 1 : 0;

    last_drawn_cursor_x = -1;
    last_drawn_cursor_y = -1;
}

void Console::put_char(char c){
    if (!check_ready()) return;

    if (c == '\r'){
        cursor_x = 0;
        draw_cursor();
        return;
    }
    if (c == '\n'){
        newline(); 
        flush(dctx);
        return;
    }
    if (cursor_x >= columns) newline();

    char* line = row_data + (((scroll_row_offset + cursor_y) % rows) * columns);
    line[cursor_x] = c;
    if (cursor_x + 1 <columns) line[cursor_x + 1] = 0;

    fb_draw_char(dctx, cursor_x * char_width, (cursor_y * line_height)+(line_height/2), c, char_scale, text_color);
    cursor_x++;
    draw_cursor();
}

//TODO: generalize this function to handle movement in general
void Console::delete_last_char(){
    if (!check_ready()) return;

    if (cursor_x > 0){
        cursor_x--;
    } else if (cursor_y > 0){
        cursor_y--;
        char* line0 = row_data + (((scroll_row_offset + cursor_y) % rows) * columns);
        cursor_x = line_len(line0, columns);
        draw_cursor();
        flush(dctx);
        return;
    } else return;

    char* line = row_data + (((scroll_row_offset + cursor_y) % rows) * columns);
    line[cursor_x] = 0;
    fb_fill_rect(dctx, cursor_x*char_width, cursor_y * line_height, char_width, line_height, bg_color);
    draw_cursor();
    flush(dctx);
}

void Console::draw_cursor(){
    if (last_drawn_cursor_x >= 0 && last_drawn_cursor_y >= 0){
        if ((uint32_t)last_drawn_cursor_x < columns && (uint32_t)last_drawn_cursor_y < rows){
            fb_fill_rect(dctx, last_drawn_cursor_x*char_width, last_drawn_cursor_y * line_height, char_width, line_height, bg_color);
            char *prev_line = row_data + (((scroll_row_offset + (uint32_t)last_drawn_cursor_y) % rows) * columns);
            char ch = prev_line[last_drawn_cursor_x];
            if (ch) fb_draw_char(dctx, last_drawn_cursor_x * char_width, (last_drawn_cursor_y * line_height)+(line_height/2), ch, char_scale, text_color);
        }
    }
    fb_fill_rect(dctx, cursor_x*char_width, cursor_y * line_height, char_width, line_height, text_color);
    last_drawn_cursor_x = (int32_t)cursor_x;
    last_drawn_cursor_y = (int32_t)cursor_y;
}

void Console::put_string(const char* str){
    if (!check_ready()) return;
    for (uint32_t i = 0; str[i]; i++) put_char(str[i]);
    flush(dctx);
}

void Console::newline(){
    if (!check_ready()) return;
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= rows - 1){
        scroll();
        cursor_y = rows - 1;
    } else {
        char* line = row_data + (((scroll_row_offset + cursor_y) % rows) * columns);
        memset(line, 0, columns);
    }
}

void Console::scroll(){
    if (!check_ready()) return;

    scroll_row_offset = (scroll_row_offset + 1) % rows;
    uint32_t clear_index = (scroll_row_offset + rows - 1) % rows;
    char* line = row_data + clear_index * columns;
    memset(line, 0, columns);

    fb_clear(dctx, bg_color);
    for (uint32_t y = 0; y < rows; y++) {
        char* l = row_data + (((scroll_row_offset + y) % rows) * columns);
        uint32_t len = line_len(l, columns);
        if (!len) continue;
        uint32_t ypix = (y * line_height) + (line_height / 2);
        for (uint32_t x = 0; x < len; x++) {
            fb_draw_char(dctx, x * char_width, ypix, l[x], char_scale, text_color);
        }
    }
    draw_cursor();
}

void Console::refresh(){
    if (!check_ready()) return;
    resize();
    screen_clear();
    redraw();
    flush(dctx);
}

void Console::redraw(){
    if (!check_ready()) return;

    for (uint32_t y = 0; y < rows; y++){
        char* line = row_data + (((scroll_row_offset + y) % rows) *columns);
        uint32_t len = line_len(line, columns);
        if (!len) continue;
        uint32_t ypix = (y * line_height) + (line_height / 2);
        for (uint32_t x = 0; x < len; x++){
            fb_draw_char(dctx, x * char_width, ypix, line[x], char_scale, text_color);
        }
    }
    draw_cursor();
}

void Console::screen_clear(){
    if (dctx) fb_clear(dctx, bg_color);
    last_drawn_cursor_x = -1;
    last_drawn_cursor_y = -1;
}

void Console::clear(){
    screen_clear();
    if (row_data && buffer_data_size) memset(row_data, 0, buffer_data_size);
    cursor_x = cursor_y = 0;
}

const char* Console::get_current_line(){
    return row_data + (((scroll_row_offset + cursor_y) % rows) * columns);
}

void Console::set_text_color(uint32_t hex){
    text_color = hex | 0xFF000000;
}