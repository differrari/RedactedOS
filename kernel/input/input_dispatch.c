#include "input_dispatch.h"
#include "process.h"

process_t* focused_proc;

void register_keypress(keypress kp) {
    input_buffer_t* buf = &focused_proc->input_buffer;
    uint32_t next_index = (buf->write_index + 1) % INPUT_BUFFER_CAPACITY;

    buf->entries[buf->write_index] = kp;
    buf->write_index = next_index;

    if (buf->write_index == buf->read_index)
        buf->read_index = (buf->read_index + 1) % INPUT_BUFFER_CAPACITY;
}

void sys_subscribe_shortcut(keypress kp){

}

void sys_set_focus(int pid){

}

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure){

}