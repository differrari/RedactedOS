#include "terminal.hpp"
#include "std/std.h"
#include "input_keycodes.h"

Terminal::Terminal() : Console() {
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

    put_string("> ");
    redraw_input_line();
    if (dirty) {
        flush(dctx);
        dirty = false;
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
        dirty =false;
    }
}

void Terminal::cursor_set_visible(bool visible){
    if (visible == cursor_visible) return;
    cursor_visible = visible;

    uint32_t cw = (uint32_t)char_scale * CHAR_SIZE;
    uint32_t lh = (uint32_t)char_scale * CHAR_SIZE*2;

    if (last_drawn_cursor_x >= 0 && last_drawn_cursor_y >= 0) {
        if ((uint32_t)last_drawn_cursor_x < columns && (uint32_t)last_drawn_cursor_y < rows) {
            fb_fill_rect(dctx, (uint32_t)last_drawn_cursor_x * cw, (uint32_t)last_drawn_cursor_y * lh, cw, lh, 0);

            char *prev_line = row_data + (((scroll_row_offset + (uint32_t)last_drawn_cursor_y) % rows) * columns);
            char ch = prev_line[last_drawn_cursor_x];
            if (ch) {
                uint32_t py = ((uint32_t)last_drawn_cursor_y * lh) + (lh / 2);
                fb_draw_char(dctx, (uint32_t)last_drawn_cursor_x * cw, py, ch, char_scale, text_color);
            }
        }
        last_drawn_cursor_x = -1;
        last_drawn_cursor_y = -1;
    }

    if (cursor_visible) {
        fb_fill_rect(dctx, cursor_x * cw, cursor_y * lh, cw, lh, 0xFFFFFFFF);
        last_drawn_cursor_x = (int32_t)cursor_x;
        last_drawn_cursor_y = (int32_t)cursor_y;
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

    fb_fill_rect(dctx, 0, cursor_y * lh, columns * cw, lh, 0);

    char* line = row_data + (((scroll_row_offset + cursor_y) % rows) * columns);
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

    uint32_t ypix = (cursor_y * lh) + (lh / 2);
    fb_draw_char(dctx, 0, ypix, '>', char_scale, text_color);
    fb_draw_char(dctx, cw, ypix, ' ', char_scale, text_color);
    for (uint32_t i = 0; i < draw_len; i++) fb_draw_char(dctx, (prompt_length + i) * cw, ypix, input_buf[i], char_scale, text_color);

    if (input_cursor > draw_len) input_cursor = draw_len;
    cursor_x = (uint32_t)prompt_length + input_cursor;

    last_blink_ms = get_time();
    if (!cursor_visible) cursor_visible = true;

    last_drawn_cursor_x = -1;
    last_drawn_cursor_y = -1;
    cursor_set_visible(true);

    dirty = true;
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
    put_char('\r');
    put_char('\n');
    put_string("> ");
    prompt_length = 2;

    set_input_line("");
    set_text_color(default_text_color);
}

bool Terminal::exec_cmd(const char *cmd, int argc, const char *argv[]){
    uint16_t proc = exec(cmd, argc, argv);
    if (!proc) return false;
    string s1 = string_format("/proc/%i/out",proc);
    string s2 = string_format("/proc/%i/state",proc);
    file out_fd, state_fd;
    fopen(s1.data, &out_fd);
    free(s1.data, s1.mem_length);
    fopen(s2.data, &state_fd);
    free(s2.data, s2.mem_length);
    int state;
    fread(&state_fd, (char*)&state, sizeof(int));
    while (state) {
        fread(&state_fd, (char*)&state, sizeof(int));
        size_t amount = 0x100;
        char *buf = (char*)malloc(amount);
        fread(&out_fd, buf, amount);
        put_string(buf);
        free(buf, amount);
    }
    fclose(&out_fd);
    fclose(&state_fd);
    string exit_msg = string_format("\nProcess %i ended.",proc);
    //TODO: format message
    put_string(exit_msg.data);
    free(exit_msg.data, exit_msg.mem_length);
    return true;
}

const char** Terminal::parse_arguments(char *args, int *count){
    *count = 0;
    const char **argv = (const char**)malloc(16 * sizeof(uintptr_t));
    char* p = args;
    while (*p && *count < 16){
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char* start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
        argv[*count] = start;
        (*count)++;
    }
    return argv;
}

void Terminal::run_command(){
    if (input_len) {
        if (history_len == history_max) {
            if (history[0]) free(history[0], strlen(history[0], input_max) + 1);
            for (uint32_t i = 1; i < history_max; i++) history[i - 1] = history[i];
            history_len = history_max - 1;
        }
        uint32_t n = input_len;
        char *copy = (char*)malloc(n + 1);
        if (copy) {
            memcpy(copy, input_buf, n);
            copy[n] = 0;
            history[history_len++] = copy;
        }
    }
    history_index = history_len;

    const char* fullcmd = input_buf;
    while (*fullcmd == ' ' || *fullcmd == '\t') fullcmd++;
    put_char('\r');
    put_char('\n');
    if (*fullcmd == 0) {
        command_running = true;
        return;
    }

    const char* args = fullcmd;
    while (*args && *args != ' ' && *args != '\t') args++;

    string cmd;
    int argc = 0;
    const char** argv = nullptr;
    string args_copy = {};

    if (*args == '\0'){
        cmd = string_from_literal(fullcmd);
    } else {
        size_t cmd_len = (size_t)(args - fullcmd);
        cmd = string_from_literal_length(fullcmd, cmd_len);
        const char* argstart = args;
        while (*argstart == ' ' || *argstart == '\t') argstart++;
        args_copy = string_from_literal(argstart);
        argv = parse_arguments(args_copy.data, &argc);
    }

    if (!exec_cmd(cmd.data, argc, argv)){
        if (strcmp(cmd.data, "test", true) == 0){
            TMP_test(argc, argv);
        } else if (strcmp(cmd.data, "exit", true) == 0){
            halt(0);
        } else {
            string s = string_format("Unknown command %s with args %s", cmd.data, args);
            put_string(s.data);
            free(s.data, s.mem_length);
        }
    }

    free(cmd.data, cmd.mem_length);
    if (args_copy.mem_length) free(args_copy.data, args_copy.mem_length);

    command_running = true;
}

void Terminal::TMP_test(int argc, const char* args[]){
    // const char *term = seek_to(args, '\033');
    // if (*term == 0) return;
    const char *term = seek_to(*args, '[');
    if (*term == 0) return;
    const char *next = seek_to(term, ';');
    uint64_t color = parse_hex_u64(term, next - term);
    set_text_color(color & UINT32_MAX);
    put_string(next);
}

bool Terminal::handle_input(){
    kbd_event event;
    if (!read_event(&event)) return false;
    if (event.type != KEY_PRESS) return false;

    char key = event.key;
    char readable = hid_to_char((uint8_t)key);
    if (key == KEY_ENTER || key == KEY_KPENTER){
        run_command();
        return true;
    }

    if (key == KEY_LEFT) {
        if (input_cursor) input_cursor--;
        cursor_x = (uint32_t)prompt_length + input_cursor;
        last_blink_ms = get_time();
        cursor_set_visible(true);
        return true;
    }

    if (key == KEY_RIGHT) {
        if (input_cursor < input_len) input_cursor++;
        cursor_x = (uint32_t)prompt_length + input_cursor;
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
    if (dctx) free(dctx, sizeof(draw_ctx));
    draw_ctx *ctx = (draw_ctx*)malloc(sizeof(draw_ctx));
    request_draw_ctx(ctx);
    return ctx;
}

void Terminal::flush(draw_ctx *ctx){
    commit_draw_ctx(ctx);
}

bool Terminal::screen_ready(){
    return true;
}
