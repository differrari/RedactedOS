#include "terminal.hpp"
#include "input/input_dispatch.h"
#include "../kio.h"
#include "../serial/uart.h"
#include "std/std.hpp"
#include "filesystem/filesystem.h"

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
}

const char* Terminal::seek_to(const char *string, char character){
    while (*string != character && *string != '\0')
        string++;
    string++;
    return string;
}

void Terminal::TMP_cat(const char *args){
    file fd;
    if (open_file(args, &fd) != FS_RESULT_SUCCESS) {
        string s = string_format("Path not found %s", args);
        put_string(s.data);
        free(s.data,s.mem_length);
        return;
    }
    size_t req_size = 0x100;
    char* buf = (char*)malloc(req_size);
    if (read_file(&fd, buf, req_size) == 0){
        put_string("Error reading file");
        return;
    }
    put_string(buf);
}

void Terminal::run_command(){
    const char* fullcmd = get_current_line();
    const char* args = seek_to(fullcmd, ' ');
    string cmd = string_ca_max(fullcmd, args - fullcmd - 1);
    string s = string_format("Executing command %s with args %s", cmd.data, args);

    put_char('\r');
    put_char('\n');

    if (strcmp(cmd.data, "cat", true) == 0){
        TMP_cat(args);
    } else put_string(s.data);
    
    free(s.data, s.mem_length);
    free(cmd.data, cmd.mem_length);
    
    draw_cursor();
    gpu_flush();
    command_running = true;
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

