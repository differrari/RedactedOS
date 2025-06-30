#include "input_dispatch.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "dwc2.hpp"
#include "hw/hw.h"

process_t* focused_proc;

typedef struct {
    keypress kp;
    int pid;
    bool triggered;
} shortcut;

shortcut shortcuts[16];

uint16_t shortcut_count = 0;

bool secure_mode = false;

DWC2Driver dwc_driver;

void register_keypress(keypress kp) {

    if (!secure_mode){
        for (int i = 0; i < shortcut_count; i++){
            if (shortcuts[i].pid != -1 && !is_new_keypress(&shortcuts[i].kp, &kp)){
                shortcuts[i].triggered = true;
                return;
            }
        }
    }

    if (!(uintptr_t)focused_proc) return;

    input_buffer_t* buf = &focused_proc->input_buffer;
    uint32_t next_index = (buf->write_index + 1) % INPUT_BUFFER_CAPACITY;

    buf->entries[buf->write_index] = kp;
    buf->write_index = next_index;

    if (buf->write_index == buf->read_index)
        buf->read_index = (buf->read_index + 1) % INPUT_BUFFER_CAPACITY;
}

uint16_t sys_subscribe_shortcut_current(keypress kp){
    return sys_subscribe_shortcut(get_current_proc_pid(),kp);
}

uint16_t sys_subscribe_shortcut(uint16_t pid, keypress kp){
    shortcuts[shortcut_count] = (shortcut){
        .kp = kp,
        .pid = pid
    };
    return shortcut_count++;
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

void sys_set_secure(bool secure){
    secure_mode = secure;
}

bool sys_read_input_current(keypress *out){
    return sys_read_input(get_current_proc_pid(), out);
}

bool is_new_keypress(keypress* current, keypress* previous) {
    if (current->modifier != previous->modifier) return true;

    for (int i = 0; i < 6; i++)
        if (current->keys[i] != previous->keys[i]) return true;

    return false;
}

bool sys_read_input(int pid, keypress *out){
    if (BOARD_TYPE == 2)
        dwc_driver.poll_interrupt_in();
    process_t *process = get_proc_by_pid(pid);
    if (process->input_buffer.read_index == process->input_buffer.write_index) return false;

    *out = process->input_buffer.entries[process->input_buffer.read_index];
    process->input_buffer.read_index = (process->input_buffer.read_index + 1) % INPUT_BUFFER_CAPACITY;
    return true;
}

bool sys_shortcut_triggered_current(uint16_t sid){
    return sys_shortcut_triggered(get_current_proc_pid(), sid);
}

bool sys_shortcut_triggered(uint16_t pid, uint16_t sid){
    if (shortcuts[sid].pid == pid && shortcuts[sid].triggered){
        shortcuts[sid].triggered = false;
        return true;
    }
    return false;
}

bool input_init(){
    if (BOARD_TYPE == 2){
        return dwc_driver.init();
    } else return xhci_input_init();
}