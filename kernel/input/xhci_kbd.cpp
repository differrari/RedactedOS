#include "xhci_kbd.h"
#include "memory/kalloc.h"
#include "console/kio.h"
#include "input_dispatch.h"
#include "memory/page_allocator.h"
#include "xHCIManager.hpp"
#include "xHCIKeyboard.hpp"

xHCIManager xhci_manager;

void xhci_kbd_request_data() {
    xhci_manager.default_device->request_data();
}

void xhci_read_key() {
    if (xhci_manager.default_device){
        xhci_manager.default_device->process_data();
    }
}

void xhci_configure_keyboard(xhci_usb_device *device){
    xhci_manager.default_device = new xHCIKeyboard();
    xhci_manager.default_device->device = device;
}