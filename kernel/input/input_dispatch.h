#pragma once

#include "xhci_kbd.h"
#include "input_keycodes.h"

void register_keypress(keypress kp);

#ifdef __cplusplus
extern "C" {
#endif 
uint16_t sys_subscribe_shortcut(uint16_t pid, keypress kp);
uint16_t sys_subscribe_shortcut_current(keypress kp);
void sys_set_focus(int pid);
void sys_focus_current();
void sys_unset_focus();

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure);

bool sys_read_input(int pid, keypress *out);
bool sys_read_input_current(keypress *out);

bool identical_keypress(keypress* current, keypress* previous);

bool sys_shortcut_triggered_current(uint16_t sid);
bool sys_shortcut_triggered(uint16_t pid, uint16_t sid);

#ifdef __cplusplus
}
#endif 