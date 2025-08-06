#include "terminal.hpp"
#include "input/input_dispatch.h"
#include "../kio.h"
#include "../serial/uart.h"

void Terminal::handle_input(){
    keypress kp;
    if (sys_read_input_current(&kp)){
        for (int i = 0; i < 6; i++){
            char key = kp.keys[i];
            // kprintf("Key[%i] %i", i, key);
            char readable = hid_to_char((uint8_t)key);
            if (readable){
                put_char(readable);
                draw_cursor();
                gpu_flush();
            } else if (key == KEY_BACKSPACE){
                uart_puts("Backspace");
                delete_last_char();
            }
        }
    }
}

