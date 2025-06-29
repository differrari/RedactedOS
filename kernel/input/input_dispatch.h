#pragma once

#include "xhci_bridge.h"
#include "input_keycodes.h"

#ifdef __cplusplus
extern "C" {
#endif 
void register_keypress(keypress kp);
uint16_t sys_subscribe_shortcut(uint16_t pid, keypress kp);
uint16_t sys_subscribe_shortcut_current(keypress kp);
void sys_set_focus(int pid);
void sys_focus_current();
void sys_unset_focus();

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure);

bool sys_read_input(int pid, keypress *out);
bool sys_read_input_current(keypress *out);

bool sys_shortcut_triggered_current(uint16_t sid);
bool sys_shortcut_triggered(uint16_t pid, uint16_t sid);

bool is_new_keypress(keypress* current, keypress* previous);

bool input_init();

#ifdef __cplusplus
}
#endif 