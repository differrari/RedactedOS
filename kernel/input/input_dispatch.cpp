#include "input_dispatch.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "dwc2.hpp"
#include "xhci.hpp"
#include "hw/hw.h"
#include "std/std.hpp"
#include "kernel_processes/kprocess_loader.h"

process_t* focused_proc;

typedef struct {
    keypress kp;
    int pid;
    bool triggered;
} shortcut;

shortcut shortcuts[16];

uint16_t shortcut_count = 0;

bool secure_mode = false;

USBDriver *input_driver = 0x0;

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
        .pid = pid,
        .triggered = false
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

void remove_double_keypresses(keypress* current, keypress* previous){
    for (int i = 0; i < 6; i++)
        if (keypress_contains(previous, current->keys[i], previous->modifier)) current->keys[i] = 0;
}

bool keypress_contains(keypress *kp, char key, uint8_t modifier){
    if (kp->modifier != modifier) return false;//TODO: This is not entirely accurate, some modifiers do not change key

    for (int i = 0; i < 6; i++)
        if (kp->keys[i] == key)
            return true;
    return false;
}

bool sys_read_input(int pid, keypress *out){
    process_t *process = get_proc_by_pid(pid);
    if (process->input_buffer.read_index == process->input_buffer.write_index) return false;

    *out = process->input_buffer.entries[process->input_buffer.read_index];
    process->input_buffer.read_index = (process->input_buffer.read_index + 1) % INPUT_BUFFER_CAPACITY;
    return true;
}

bool sys_shortcut_triggered_current(uint16_t sid){
    bool value = sys_shortcut_triggered(get_current_proc_pid(), sid);
    return value;
}

bool sys_shortcut_triggered(uint16_t pid, uint16_t sid){
    if (shortcuts[sid].pid == pid && shortcuts[sid].triggered){
        shortcuts[sid].triggered = false;
        return true;
    } 
    return false;
}

bool input_init(){
    for (int i = 0; i < 16; i++) shortcuts[i] = (shortcut){0};
    if (BOARD_TYPE == 2 && RPI_BOARD != 5){
        input_driver = new DWC2Driver();//TODO: QEMU & 3 Only
        return input_driver->init();
    } else {
        input_driver = new XHCIDriver();
        return input_driver->init();
    }
}

void input_process_poll(){
    while (1){
        input_driver->poll_inputs();
    }
}

void input_process_fake_interrupts(){
    while (1){
        input_driver->handle_interrupt();
    }
}

void init_input_process(){
    if (!input_driver->use_interrupts)
        create_kernel_process("input_poll", &input_process_poll);
    if (input_driver->quirk_simulate_interrupts)
        create_kernel_process("input_int_mock", &input_process_fake_interrupts);
}

void handle_input_interrupt(){
    if (input_driver->use_interrupts) input_driver->handle_interrupt();
}

driver_module input_module = (driver_module){
    .name = "input",
    .mount = "/in",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = input_init,
    .fini = 0,
    .open = 0,
    .read = 0,
    .write = 0,
    .seek = 0,
    .readdir = 0,
};