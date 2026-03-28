#pragma once

#include "input_keycodes.h"
#include "keyboard_input.h"
#include "mouse_input.h"
#include "dev/driver_base.h"
#include "ui/graphic_types.h"

#ifdef __cplusplus
extern "C" {
#endif 

typedef enum mouse_button {
    LMB,
    RMB,
    MMB,
} mouse_button;

bool register_keypress(keypress kp);
void register_event(kbd_event event);
void mouse_config(gpu_point point, gpu_size size);
void register_mouse_input(mouse_input *rat);

mouse_input get_raw_mouse_in();

gpu_point get_mouse_pos();
bool mouse_button_pressed(mouse_button mb);

uint16_t sys_subscribe_shortcut(uint16_t pid, keypress kp);
uint16_t sys_subscribe_shortcut_current(keypress kp);
void sys_set_focus(int pid);
void sys_focus_current();
void sys_unset_focus(bool close);

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure);

bool sys_read_input(int pid, keypress *out);
bool sys_read_input_current(keypress *out);

bool sys_read_event(int pid, kbd_event *out);
bool sys_read_event_current(kbd_event *out);

bool sys_shortcut_triggered_current(uint16_t sid);
bool sys_shortcut_triggered(uint16_t pid, uint16_t sid);

bool is_new_keypress(keypress* current, keypress* previous);
bool keypress_contains(keypress *kp, char key, uint8_t modifier);
void remove_double_keypresses(keypress* current, keypress* previous);

#ifdef __cplusplus
}
#endif 