#pragma once

#include "xhci_kbd.h"

void register_keypress(keypress kp);

#define KEY_ARROW_UP 0x52
#define KEY_ARROW_DOWN 0x51
#define KEY_ARROW_LEFT 0x50
#define KEY_ARROW_RIGHT  0x4F
#define KEY_BACKSPACE 0x28
#define KEY_ENTER 0x2A

#ifdef __cplusplus
extern "C" {
#endif 
void sys_subscribe_shortcut(keypress kp);
void sys_set_focus(int pid);
void sys_focus_current();
void sys_unset_focus();

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure);

bool sys_read_input(int pid, keypress *out);
bool sys_read_input_current(keypress *out);

bool identical_keypress(keypress* current, keypress* previous);

#ifdef __cplusplus
}
#endif 