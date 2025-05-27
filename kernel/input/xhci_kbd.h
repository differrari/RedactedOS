#pragma once

#ifdef __cplusplus
extern "C" {
#endif 

#include "xhci.h"
#include "xhci_types.h"

#include "keypress.h"

void set_default_kbd(xhci_usb_device* kbd);
void xhci_kbd_request_data();
keypress xhci_read_key();

bool is_new_keypress(keypress* current, keypress* previous);

void xhci_configure_keyboard(xhci_usb_device *device);

#ifdef __cplusplus
}
#endif 