#pragma once

#include "xhci_kbd.h"

void register_keypress(keypress kp);

void sys_subscribe_shortcut(keypress kp);
void sys_set_focus(int pid);
void sys_focus_current();

void sys_unset_focus();

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure);

bool sys_read_input(int pid, keypress *out);
bool sys_read_input_current(keypress *out);

bool identical_keypress(keypress* current, keypress* previous);