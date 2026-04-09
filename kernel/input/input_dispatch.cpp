#include "input_dispatch.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "math/math.h"
#include "graph/graphics.h"
#include "graph/tres.h"

process_t* focused_proc;

typedef struct {
    keypress kp;
    int pid;
    bool triggered;
} shortcut;

shortcut shortcuts[16] = {};
static const u16 shortcut_capacity = sizeof(shortcuts) / sizeof(shortcuts[0]);

u16 shortcut_count = 0;

bool secure_mode = false;

gpu_point mouse_loc;
gpu_size screen_bounds;

bool mouse_setup;

bool register_keypress(keypress kp) {
    if (!secure_mode){
        for (u16 i = 0; i < shortcut_count; i++){
            if (!is_new_keypress(&shortcuts[i].kp, &kp)){
                shortcuts[i].triggered = true;
                return true;
            }
        }
    }

    process_t *target = focused_proc;
    if (!target || target->state == process::STOPPED || !target->id || !target->pc || !target->sp || (((target->spsr & 0xF) == 0) && !target->mm.ttbr0)) {
        u16 win_id = target ? target->win_id : 0;
        u16 skip_id = target ? target->id : 0;
        focused_proc = 0;

        if (win_id) {
            u16 pid = window_fallback_focus(win_id, skip_id);
            if (pid) focused_proc = get_proc_by_pid(pid);
        }

        target = focused_proc;
        if (!target || target->state == process::STOPPED || !target->id || !target->pc || !target->sp || (((target->spsr & 0xF) == 0) && !target->mm.ttbr0)) {
            focused_proc = 0;
            return false;
        }
    }

    input_buffer_t* buf = &target->input_buffer;
    uint32_t next_index = (buf->write_index + 1) % INPUT_BUFFER_CAPACITY;

    buf->entries[buf->write_index] = kp;
    buf->write_index = next_index;

    if (buf->write_index == buf->read_index)
        buf->read_index = (buf->read_index + 1) % INPUT_BUFFER_CAPACITY;
    
    return false;
}

bool register_scroll(i8 scroll){
    if (!(uintptr_t)focused_proc) return false;
    
    scroll_buffer_t* buf = &focused_proc->scroll_buffer;
    
    uint32_t next_index = (buf->write_index + 1) % INPUT_BUFFER_CAPACITY;

    buf->entries[buf->write_index] = scroll;
    buf->write_index = next_index;

    if (buf->write_index == buf->read_index)
        buf->read_index = (buf->read_index + 1) % INPUT_BUFFER_CAPACITY;
    
    return true;
}

void register_event(kbd_event event){
    process_t *target = focused_proc;
    if (!target || target->state == process::STOPPED || !target->id || !target->pc || !target->sp || (((target->spsr & 0xF) == 0) && !target->mm.ttbr0)) {
        u16 win_id = target ? target->win_id : 0;
        u16 skip_id = target ? target->id : 0;
        focused_proc = 0;

        if (win_id) {
            u16 pid = window_fallback_focus(win_id, skip_id);
            if (pid) focused_proc = get_proc_by_pid(pid);
        }

        target = focused_proc;
        if (!target || target->state == process::STOPPED || !target->id || !target->pc || !target->sp || (((target->spsr & 0xF) == 0) && !target->mm.ttbr0)) {
            focused_proc = 0;
            return;
        }
    }

    event_buffer_t* buf = &target->event_buffer;
    uint32_t next_index = (buf->write_index + 1) % INPUT_BUFFER_CAPACITY;

    buf->entries[buf->write_index] = event;
    buf->write_index = next_index;

    if (buf->write_index == buf->read_index)
        buf->read_index = (buf->read_index + 1) % INPUT_BUFFER_CAPACITY;
}

void mouse_config(gpu_point point, gpu_size size){
    mouse_loc = point;
    screen_bounds = size;
    gpu_setup_cursor(point);
    mouse_setup = true;
}

uint8_t last_cursor_state = 0;
mouse_input last_mouse_in;

mouse_input get_raw_mouse_in(){
    return last_mouse_in;
}

void register_mouse_input(mouse_input *rat){
    last_mouse_in = *rat;
    if (!mouse_setup) return;
    int32_t dx = rat->x;
    int32_t dy = rat->y;
    mouse_loc.x += dx;
    mouse_loc.y += dy;
    mouse_loc.x = min(max(0, mouse_loc.x), screen_bounds.width);
    mouse_loc.y = min(max(0, mouse_loc.y), screen_bounds.height);
    gpu_update_cursor(mouse_loc, false);
    register_scroll(rat->scroll);
    uint8_t cursor_state = rat->buttons;
    if (cursor_state != last_cursor_state){
        last_cursor_state = cursor_state;
        gpu_set_cursor_pressed(last_cursor_state);
        gpu_update_cursor(mouse_loc, true);
    }
}

gpu_point get_mouse_pos(){
    return mouse_loc;
}

bool mouse_button_pressed(int mb){
    return (last_cursor_state & (1 << mb)) == (1 << mb);
}

uint16_t sys_subscribe_shortcut_current(keypress kp){
    return sys_subscribe_shortcut(get_current_proc_pid(),kp);
}

uint16_t sys_subscribe_shortcut(uint16_t pid, keypress kp){
    if (shortcut_count >= shortcut_capacity) return UINT16_MAX;
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
    process_t *target = get_proc_by_pid(pid);
    if (!target || target->state == process::STOPPED || !target->id || !target->pc || !target->sp || (((target->spsr & 0xF) == 0) && !target->mm.ttbr0)) return;
    if (focused_proc) focused_proc->focused = false;
    focused_proc = target;
    focused_proc->focused = true;
    set_window_focus(focused_proc->win_id);
}

void sys_unset_focus(bool close){
    process_t *proc = focused_proc;
    if (proc) proc->focused = false;
    focused_proc = 0;
    unset_window_focus();

    u16 npid = proc && proc->win_id ? window_fallback_focus(proc->win_id, proc->id) : 0;
    if (npid)
    {
        process_t *next = get_proc_by_pid(npid);
        if (next && next->focused && next->state != process::STOPPED && next->id && next->pc && next->sp && ((((next->spsr & 0xF) != 0) || next->mm.ttbr0))) focused_proc = next;
    }
}

void sys_set_secure(bool secure){
    secure_mode = secure;
}

bool sys_read_event_current(kbd_event *out){
    return sys_read_event(get_current_proc_pid(), out);
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

bool sys_read_event(int pid, kbd_event *out){
    process_t *process = get_proc_by_pid(pid);
    if (!process) return false;
    if (process->event_buffer.read_index == process->event_buffer.write_index) return false;

    *out = process->event_buffer.entries[process->event_buffer.read_index];
    process->event_buffer.read_index = (process->event_buffer.read_index + 1) % INPUT_BUFFER_CAPACITY;
    return true;
}

i8 sys_read_scroll(int pid){
    process_t *process = get_proc_by_pid(pid);
    if (process->scroll_buffer.read_index == process->scroll_buffer.write_index) return false;

    i8 ret = process->scroll_buffer.entries[process->scroll_buffer.read_index];
    process->scroll_buffer.read_index = (process->scroll_buffer.read_index + 1) % INPUT_BUFFER_CAPACITY;
    return ret;
}

i8 sys_read_scroll_current(){
    return sys_read_scroll(get_current_proc_pid());
}

bool sys_read_input(int pid, keypress *out){
    process_t *process = get_proc_by_pid(pid);
    if (!process) return false;
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
    if (sid >= shortcut_count) return false;
    if (shortcuts[sid].pid == pid && shortcuts[sid].triggered){
        shortcuts[sid].triggered = false;
        return true;
    } 
    return false;
}
