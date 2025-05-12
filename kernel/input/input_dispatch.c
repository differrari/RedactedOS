#include "input_dispatch.h"
#include "process.h"
#include "process/scheduler.h"

process_t* focused_proc;

static const char hid_keycode_to_char[256] = {
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd',
    [0x08] = 'e', [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h',
    [0x0C] = 'i', [0x0D] = 'j', [0x0E] = 'k', [0x0F] = 'l',
    [0x10] = 'm', [0x11] = 'n', [0x12] = 'o', [0x13] = 'p',
    [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x',
    [0x1C] = 'y', [0x1D] = 'z',
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4',
    [0x22] = '5', [0x23] = '6', [0x24] = '7', [0x25] = '8',
    [0x26] = '9', [0x27] = '0',
    [0x28] = '\n', [0x2C] = ' ', [0x2D] = '-', [0x2E] = '=',
    [0x2F] = '[', [0x30] = ']', [0x31] = '\\', [0x33] = ';',
    [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.',
    [0x38] = '/',
};

void register_keypress(keypress kp) {
    input_buffer_t* buf = &focused_proc->input_buffer;
    uint32_t next_index = (buf->write_index + 1) % INPUT_BUFFER_CAPACITY;

    buf->entries[buf->write_index] = kp;
    buf->write_index = next_index;

    if (buf->write_index == buf->read_index)
        buf->read_index = (buf->read_index + 1) % INPUT_BUFFER_CAPACITY;
}

void sys_subscribe_shortcut(keypress kp){
    //Register a shortcut, as requested by a system
}

void sys_focus_current(){
    sys_set_focus(get_current_proc_pid());
}

void sys_set_focus(int pid){
    focused_proc = get_proc_by_pid(pid);
}

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure){
    //Temporarily disable shortcuts. I guess shortcuts could malitiously be used to detect parts of a keyboard
}

bool sys_read_input_current(keypress *out){
    sys_read_input(get_current_proc_pid(), out);
}

bool sys_read_input(int pid, keypress *out){
    process_t *process = get_proc_by_pid(pid);
    if (process->input_buffer.read_index == process->input_buffer.write_index) return false;

    *out = process->input_buffer.entries[process->input_buffer.read_index];
    for (int i = 0; i < 6; i++){
        out->keys[i] = hid_keycode_to_char[out->keys[i]];
    }
    process->input_buffer.read_index = (process->input_buffer.read_index + 1) % INPUT_BUFFER_CAPACITY;
    return true;
}