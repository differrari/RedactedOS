#include "terminal.hpp"
#include "alloc/allocate.h"
#include "std/std.h"
#include "input_keycodes.h"
#include "shell/sheldon/sheldon.h"
#include "files/helpers.h"
#include "data/serialize/binary_serial.h"
#include "environment/env_types.h"
#include "utils/embedded_fmt/tcf.h"

Terminal *default_term;

void term_put_char(shell_handle *handle, char c){
    if (!default_term || (default_term->term_current_shell && default_term->term_current_shell != handle)) return;
    default_term->put_char(c);
}

void term_clear(shell_handle *handle){
    if (!default_term || (default_term->term_current_shell && default_term->term_current_shell != handle)) return;
    default_term->clear();
}

void term_flush(shell_handle *handle){
    if (!default_term || (default_term->term_current_shell && default_term->term_current_shell != handle)) return;
    default_term->refresh();
}

void term_bell(shell_handle *handle){
    if (!default_term || (default_term->term_current_shell && default_term->term_current_shell != handle)) return;
    default_term->bell();
}

void term_ascii_cmd(shell_handle *handle, char cmd, u16 proc_id){
    if (!default_term || (default_term->term_current_shell && default_term->term_current_shell != handle)) return;
    default_term->interpret_cmd_code(cmd, proc_id);
}

void term_console_ctrl(shell_handle *handle, console_ctrls ctrl){
    if (!default_term || (default_term->term_current_shell && default_term->term_current_shell != handle)) return;
    default_term->ctrl(ctrl);
}

Terminal::Terminal() : Console() {
    default_term = this;
    uint32_t color_buf[2] = {};
    sreadf("/theme", &color_buf, sizeof(uint64_t));
    if ((color_buf[0] & 0xFF000000) == 0) color_buf[0] |= 0xFF000000;
    current_format.default_bg_color = color_buf[0];
    current_format.current_bg_color = color_buf[0];
    current_format.default_text_color = color_buf[1];

    char_scale = 2;
    prompt_length = 2;
    command_running = false;

    input_len = 0;
    input_cursor = 0;
    input_buf[0] = 0;

    history_len = 0;
    history_index = 0;
    for (uint32_t i = 0; i < history_max; i++) history[i] = nullptr;

    last_blink_ms = get_time();
    cursor_visible = true;

    dirty = false;

    term_current_shell = create_shell();
    init_tcf(&current_format);
    put_string("> ");
    redraw_input_line();
    if (dirty) {
        flush(dctx);
        dirty = false;
    }
}

shell_handle* Terminal::create_shell(){
    return create_sheldon((shell_bindings){
        .console_output = term_put_char,
        .console_flush = term_flush,
        .console_clean = term_clear,
        .console_bell = term_bell,
        .console_ascii_cmd = term_ascii_cmd,
        .console_control = term_console_ctrl
    }, 0);
}

void Terminal::ctrl(console_ctrls ctrl){
    switch (ctrl) {
    case console_ctrl_close: halt(0);
    default: break;
    }
}

void Terminal::update(){
    if (!command_running) {
        bool did = handle_input();
        if (!did) cursor_tick();
    } else {
        end_command();
    }

    if (dirty) {
        flush(dctx);
        dirty = false;
    }
}

void Terminal::cursor_set_visible(bool visible){
    if (visible == cursor_visible) {
        if (!visible) return;
        if (last_drawn_cursor_x == (int32_t)current_format.cursor_x && last_drawn_cursor_y == (int32_t)current_format.cursor_y) return;
    }

    cursor_visible = visible;

    if (last_drawn_cursor_x >= 0 && last_drawn_cursor_y >= 0) {
        if ((uint32_t)last_drawn_cursor_x < columns && (uint32_t)last_drawn_cursor_y < rows) {
            char *prev_line = &row_data[((scroll_row_offset + (u32)last_drawn_cursor_y) % rows) * columns];
            render_glyph(last_drawn_cursor_x*char_width, last_drawn_cursor_y * line_height, prev_line[last_drawn_cursor_x], current_format.current_text_color, current_format.current_bg_color, true);
        }
        last_drawn_cursor_x = -1;
        last_drawn_cursor_y = -1;
    }

    if (cursor_visible) {
        fb_fill_rect(dctx, current_format.cursor_x * char_width, current_format.cursor_y * line_height, char_width, line_height, 0xFFFFFFFF);
        last_drawn_cursor_x = (int32_t)current_format.cursor_x;
        last_drawn_cursor_y = (int32_t)current_format.cursor_y;
    }

    dirty = true;
}

void Terminal::cursor_tick(){
    uint64_t now = get_time();
    if ((now - last_blink_ms) < 500) return;
    last_blink_ms = now;
    cursor_set_visible(!cursor_visible);
}

void Terminal::redraw_input_line(){
    if (!check_ready()) return;

    uint32_t cw = (uint32_t)char_scale * CHAR_SIZE;
    uint32_t lh = (uint32_t)char_scale * CHAR_SIZE * 2;

    fb_fill_rect(dctx, 0, current_format.cursor_y * lh, columns * cw, lh, current_format.current_bg_color);

    char* line = row_data + (((scroll_row_offset + current_format.cursor_y) % rows) * columns);
    memset(line, 0, columns);

    if (columns == 0) return;
    if (prompt_length >= (int)columns) return;

    line[0] = '>';
    line[1] = ' ';

    uint32_t max_input = columns - (uint32_t)prompt_length - 1;
    uint32_t draw_len = input_len;
    if (draw_len > max_input) draw_len = max_input;

    for (uint32_t i = 0; i < draw_len; i++) line[prompt_length + i] = input_buf[i];
    line[prompt_length + draw_len] = 0;

    uint32_t ypix = (current_format.cursor_y * lh) + (lh / 2);
    fb_draw_char(dctx, 0, ypix, '>', char_scale, current_format.current_text_color);
    fb_draw_char(dctx, cw, ypix, ' ', char_scale, current_format.current_text_color);
    for (uint32_t i = 0; i < draw_len; i++) fb_draw_char(dctx, (prompt_length + i) * cw, ypix, input_buf[i], char_scale, current_format.current_text_color);

    if (input_cursor > draw_len) input_cursor = draw_len;
    current_format.cursor_x = (uint32_t)prompt_length + input_cursor;

    last_blink_ms = get_time();
    cursor_set_visible(true);
}

void Terminal::set_input_line(const char *s){
    input_len = 0;
    input_cursor = 0;

    if (s) {
        uint32_t i = 0;
        while (s[i] && (i + 1) < input_max) {
            input_buf[i] = s[i];
            i++;
        }
        input_len = i;
    }

    input_buf[input_len] = 0;
    input_cursor = input_len;
    redraw_input_line();
}

void Terminal::end_command(){
    command_running = false;
    if (last_char != '\r' && last_char != '\n'){
        put_char('\r');
        put_char('\n');
    }
    put_string("> ");
    prompt_length = 2;

    set_input_line("");
}

void term_emit_data(structdef field, sizedptr data, bool is_allocated){
    if (!default_term) return;
    default_term->emit_data(field,data,is_allocated);
}

void Terminal::emit_data(structdef field, sizedptr data, bool is_allocated){
    if (!data.ptr || !data.size) return;
        switch (field.type) {
        case binary_type_i8: print("%S: %i",field.name,*(i8*)data.ptr); break;
        case binary_type_i16: print("%S: %i",field.name,*(i16*)data.ptr); break;
        case binary_type_i32: print("%S: %i",field.name,*(i32*)data.ptr); break;  
        case binary_type_i64: print("%S: %i",field.name,*(i64*)data.ptr); break;
        case binary_type_float: print("%S: %f",field.name,*(float*)data.ptr); break;
        case binary_type_double: print("%S: %f",field.name,*(double*)data.ptr); break;
        case binary_type_string: print("%S: %v",field.name,data); break;
        default: return;
    }
    if (is_allocated) release((void*)data.ptr);
}

bool Terminal::exec_cmd(const char *cmd){
    if (!term_current_shell) return false;

    current_shell = term_current_shell;
    
    if (run_cmd(current_shell, slice_from_literal(cmd))) return true;
    
    return true;
}

void Terminal::bell(){
    put_string("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    refresh();
}

void Terminal::run_command(){
    if (input_len) {
        if (history_len == history_max) {
            if (history[0]) release(history[0]);
            for (uint32_t i = 1; i < history_max; i++) history[i - 1] = history[i];
            history_len = history_max - 1;
        }

        uint32_t n = input_len;
        char *copy = (char*)zalloc(n + 1);
        if (copy) {
            memcpy(copy, input_buf, n);
            copy[n] = 0;
            history[history_len++] = copy;
        }
    }
    history_index = history_len;

    put_char('\r');
    put_char('\n');
    
    const char* fullcmd = input_buf;
    while (*fullcmd == ' ' || *fullcmd == '\t') fullcmd++;

    if (*fullcmd == 0) {
        command_running = true;
        return;
    }

    if (!exec_cmd(fullcmd)){
        const char *next = seek_to(fullcmd, ' ');
        size_t len = next - fullcmd - (next && *(next-1) == ' ' ? 1 : 0);
        string_slice cmd = (string_slice){ .data = (char*)fullcmd, .length = len };
        if (slice_lit_match(cmd, "exit", true) || slice_lit_match(cmd, "q", true)){
            halt(0);
        } else {
            string s = string_format("Unknown command %v", cmd);
            put_string(s.data);
            string_free(s);
        }
    }

    command_running = true;
}

bool Terminal::interpret_cmd_code(char code, u16 proc){
    if (code == ASCII_CMD_ETX){
        send_signal(SIG_QUIT, proc);
        return true;
    }
    if (code == ASCII_CMD_SUB){
        send_signal(SIG_STOP, proc);
        return true;
    }
    if (code == ASCII_CMD_CAN){
        send_signal(SIG_CONT, proc);
    }
    return false;
}

bool Terminal::handle_input(){
    kbd_event event;
    if (!read_event(&event)) return false;
    if (event.type == KEY_RELEASE) return true;
    if (event.type != KEY_PRESS) return handle_modifier(&event);

    char key = event.key;
    char readable = hid_to_char((uint8_t)key, current_modifier);

    if (command_running){
        return interpret_cmd_code(readable, 0);
    }

    if (key == KEY_ENTER || key == KEY_KPENTER){
        run_command();
        return true;
    }

    if (key == KEY_LEFT) {
        if (input_cursor) input_cursor--;
        current_format.cursor_x = (uint32_t)prompt_length + input_cursor;
        last_blink_ms = get_time();
        cursor_set_visible(true);
        return true;
    }

    if (key == KEY_RIGHT) {
        if (input_cursor < input_len) input_cursor++;
        current_format.cursor_x = (uint32_t)prompt_length + input_cursor;
        last_blink_ms = get_time();
        cursor_set_visible(true);
        return true;
    }

    if (key == KEY_UP) {
        if (history_len && history_index) {
            history_index--;
            set_input_line(history[history_index]);
        }
        return true;
    }

    if (key == KEY_DOWN) {
        if (history_len) {
            if (history_index + 1 < history_len) {
                history_index++;
                set_input_line(history[history_index]);
            } else {
                history_index = history_len;
                set_input_line("");
            }
        }
        return true;
    }

    if (key == KEY_BACKSPACE){
        if (!input_cursor) return true;
        for (uint32_t i = input_cursor; i < input_len; i++) input_buf[i - 1] = input_buf[i];
        input_len--;
        input_cursor--;
        input_buf[input_len] = 0;
        redraw_input_line();
        return true;
    }

    if (key == KEY_DELETE) {
        if (input_cursor >= input_len) return true;
        for (uint32_t i = input_cursor + 1; i <= input_len; i++) input_buf[i - 1] = input_buf[i];
        input_len--;
        redraw_input_line();
        return true;
    }

    if (!readable) return true;

    uint32_t max_visible = 0;
    if (columns > (uint32_t)prompt_length + 1) max_visible = columns - (uint32_t)prompt_length - 1;
    if (input_len >= input_max - 1) return true;
    if (max_visible && input_len >= max_visible) return true;

    for (uint32_t i = input_len; i > input_cursor; i--) input_buf[i] = input_buf[i - 1];
    input_buf[input_cursor] = readable;
    input_len++;
    input_cursor++;
    input_buf[input_len] = 0;
    redraw_input_line();
    return true;
}

draw_ctx* Terminal::get_ctx(){
    if (dctx) release(dctx);
    draw_ctx *ctx = (draw_ctx*)zalloc(sizeof(draw_ctx));
    if (!ctx) return nullptr;
    request_draw_ctx(ctx);
    return ctx;
}

void Terminal::flush(draw_ctx *ctx){
    commit_draw_ctx(ctx);
}

void Terminal::refresh(){
    flush(dctx);
}

bool Terminal::screen_ready(){
    return true;
}
