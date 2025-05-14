#include "input_dispatch.h"
#include "process.h"
#include "process/scheduler.h"

process_t* focused_proc;

void register_keypress(keypress kp) {

    if (!(uintptr_t)focused_proc) return;

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
    focused_proc->focused = true;
}

void sys_unset_focus(){
    focused_proc->focused = false;
    focused_proc = 0;
}

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure){
    //Temporarily disable shortcuts. I guess shortcuts could malitiously be used to detect parts of a password
}

bool sys_read_input_current(keypress *out){
    return sys_read_input(get_current_proc_pid(), out);
}

bool identical_keypress(keypress* current, keypress* previous){
    return !is_new_keypress(current,previous);
}

bool sys_read_input(int pid, keypress *out){
    process_t *process = get_proc_by_pid(pid);
    if (process->input_buffer.read_index == process->input_buffer.write_index) return false;

    *out = process->input_buffer.entries[process->input_buffer.read_index];
    process->input_buffer.read_index = (process->input_buffer.read_index + 1) % INPUT_BUFFER_CAPACITY;
    return true;
}