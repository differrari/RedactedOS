#include "terminal.hpp"
#include "std/std.h"
#include "input_keycodes.h"

Terminal::Terminal() : Console() {
    char_scale = 2;
    put_string("> ");
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
    const char* prev = args;
    char* next_args;
    const char **argv = (const char**)malloc(16 * sizeof(uintptr_t));
    do {
        next_args = (char*)seek_to(args, ' ');
        argv[*count] = prev;
        (*count)++;
        prev = next_args;
        *(next_args - 1) = 0;
    } while(prev != next_args);
    if (*next_args){
        argv[*count] = prev;
        (*count)++;
    }
    return argv;
}

void Terminal::run_command(){
    const char* fullcmd = get_current_line();
    const char* args = seek_to(fullcmd, ' ');

    if (fullcmd[0] == '>' && fullcmd[1] == ' ') {
        fullcmd += 2;
    }

    string cmd;
    int argc = 0;
    const char** argv;
    string args_copy = {};

    if (fullcmd == args){
        cmd = string_from_literal(fullcmd);
        argv = 0;
    } else {
        cmd = string_from_literal_length(fullcmd, args - fullcmd - 1);
        args_copy = string_from_literal(args);
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
                delete_last_char();
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
