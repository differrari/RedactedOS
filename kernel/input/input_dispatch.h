#pragma once

#include "xhci_kbd.h"

void register_keypress(keypress kp);

void sys_subscribe_shortcut(keypress kp);
void sys_set_focus(int pid);

///A process can request for shortcuts and others to be disabled
void sys_set_secure(bool secure);