#pragma once

#include "xhci.h"
#include "xhci_types.h"

#include "keypress.h"

void set_default_kbd(xhci_usb_device* kbd);
void xhci_kbd_request_data();
keypress xhci_read_key();