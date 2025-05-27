#pragma once

#ifdef __cplusplus
extern "C" {
#endif 

#include "xhci.h"
#include "xhci_types.h"

#include "keypress.h"

void xhci_kbd_request_data();
void xhci_read_key();

void xhci_configure_keyboard(xhci_usb_device *device);

#ifdef __cplusplus
}
#endif 