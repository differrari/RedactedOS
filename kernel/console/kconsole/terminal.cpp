#include "terminal.hpp"
#include "input/input_dispatch.h"
#include "../kio.h"
#include "../serial/uart.h"
#include "std/std.hpp"
#include "filesystem/filesystem.h"
#include "bin/cat.h"

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
    gpu_flush();
    set_text_color(default_text_color);
}

void Terminal::TMP_cat(int argc, const char *args[]){
    process_t *cat = create_cat_process(argc, args);
    string s = string_format("/proc/%i/out",cat->id);
    file fd;
    open_file(s.data, &fd);
    free(s.data, s.mem_length);
    while (cat->state != process_t::STOPPED){
        size_t amount = 0x100;
        char *buf = (char*)malloc(amount);
        read_file(&fd, buf, amount);
        put_string(buf);
        free(buf, amount);
    }
    string exit_msg = string_format("Process %i ended with exit code %i.",cat->id, cat->exit_code);
    put_string(exit_msg.data);
    free(exit_msg.data, exit_msg.mem_length);
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
        kprintf("Found an argument %s",prev);
    } while(prev != next_args);
    if (*next_args){
        argv[*count] = prev;
        (*count)++;
        kprintf("Ended at %s",next_args);
    }
    return argv;
}

void Terminal::run_command(){
    const char* fullcmd = get_current_line();
    const char* args = seek_to(fullcmd, ' ');
    
    string cmd;
    int argc = 0;
    const char** argv; 
    string args_copy;
    
    if (fullcmd == args){
        cmd = string_l(fullcmd);
    } else {
        cmd = string_ca_max(fullcmd, args - fullcmd - 1);
        args_copy = string_l(args);
        argv = parse_arguments(args_copy.data, &argc);
    }

    put_char('\r');
    put_char('\n');

    if (strcmp(cmd.data, "cat", true) == 0)
        TMP_cat(argc, argv);
    else if (strcmp(cmd.data, "test", true) == 0)
        TMP_test(argc, argv);
    else {
        string s = string_format("Unknown command %s with args %s", cmd.data, args);
        put_string(s.data);
        free(s.data, s.mem_length);
    }
    
    free(cmd.data, cmd.mem_length);
    if (args_copy.mem_length) free(args_copy.data, args_copy.mem_length);
    
    draw_cursor();
    gpu_flush();
    command_running = true;
}

//TODO: implement the full state machine explained at https://vt100.net/emu/dec_ansi_parser & https://en.wikipedia.org/wiki/ANSI_escape_code#Colors
//The current implementation is not standard compliant and uses hex colors as [FF0000;
void Terminal::TMP_test(int argc, const char* args[]){
    // const char *term = seek_to(args, '\033');
    // if (*term == 0) return;
    const char *term = seek_to(*args, '[');
    if (*term == 0) return;
    const char *next = seek_to(term, ';');
    uint64_t color = parse_hex_u64(term, next - term);
    set_text_color(color & 0xFFFFFF);
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
                gpu_flush();
            } else if (key == KEY_BACKSPACE){
                delete_last_char();
            }
        }
    }
}

