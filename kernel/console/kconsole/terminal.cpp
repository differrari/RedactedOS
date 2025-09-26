#include "terminal.hpp"
#include "input/input_dispatch.h"
#include "std/std.h"
#include "filesystem/filesystem.h"
#include "bin/bin_mod.h"

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
    draw_cursor();
    flush(dctx);
    set_text_color(default_text_color);
}

bool Terminal::exec_cmd(const char *cmd, int argc, const char *argv[]){
    process_t *proc = exec(cmd, argc, argv);
    if (!proc) return false;
    string s = string_format("/proc/%i/out",proc->id);
    file fd;
    open_file(s.data, &fd);
    free(s.data, s.mem_length);
    while (proc->state != process_t::STOPPED){
        size_t amount = 0x100;
        char *buf = (char*)malloc(amount);
        read_file(&fd, buf, amount);
        put_string(buf);
        free(buf, amount);
    }
    close_file(&fd);
    string exit_msg = string_format("Process %i ended with exit code %i.",proc->id, proc->exit_code);
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
    keypress kp;
    if (sys_read_input_current(&kp)){
        for (int i = 0; i < 6; i++){
            char key = kp.keys[i];
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
