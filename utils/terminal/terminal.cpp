#include "terminal.hpp"
#include "std/std.h"
#include "input_keycodes.h"

Terminal::Terminal() : Console() {
    char_scale = 2;
    put_string("> ");
    prompt_length = 2;
    draw_cursor();
    flush(dctx);
}

void Terminal::update(){
    if (!command_running) handle_input();
    else {
        end_command();
    }
}

void Terminal::end_command(){
    command_running = false;
    put_char('\r');
    put_char('\n');
    put_string("> ");
    prompt_length = 2;
    draw_cursor();
    flush(dctx);
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
    int state = 1;
    size_t amount = 0x100;
    char *buf = (char*)malloc(amount);
    do {
        fread(&out_fd, buf, amount);
        put_string(buf);
        fread(&state_fd, (char*)&state, sizeof(int));
    } while (state);
    free(buf, amount);
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
    const char* fullcmd = get_current_line();
    if (fullcmd[0] == '>' && fullcmd[1] == ' ') {
        fullcmd += 2;
    }
    while (*fullcmd == ' ' || *fullcmd == '\t') fullcmd++;
    if (*fullcmd == '\0') {
        put_char('\r');
        put_char('\n');
        put_string("> ");
        prompt_length = 2;
        draw_cursor();
        flush(dctx);
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

    put_char('\r');
    put_char('\n');

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

    draw_cursor();
    flush(dctx);
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

void Terminal::handle_input(){
    kbd_event event;
    if (read_event(&event)){
        if (event.type == KEY_PRESS){
            char key = event.key;
            char readable = hid_to_char((uint8_t)key);
            if (key == KEY_ENTER || key == KEY_KPENTER){
                run_command();
            } else if (readable){
                put_char(readable);
                draw_cursor();
                flush(dctx);
            } else if (key == KEY_BACKSPACE){
                if (strlen_max(get_current_line(), 1024) > (uint32_t)prompt_length) {
                    delete_last_char();
                }
            }
        }
    }
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
