#include "xhci_bridge.h"
#include "memory/kalloc.h"
#include "console/kio.h"
#include "input_dispatch.h"
#include "memory/page_allocator.h"
#include "xHCIManager.hpp"
#include "xHCIKeyboard.hpp"

xHCIManager *xhci_manager;

void xhci_bridge_request_data(uint8_t slot_id, uint8_t endpoint_id) {
    xhci_manager->default_device->request_data(endpoint_id);
}

void xhci_process_input(uint8_t slot_id, uint8_t endpoint_id){
    if (xhci_manager->default_device){
        xhci_manager->default_device->process_data(endpoint_id);
    }
}

void xhci_initialize_manager(uint32_t capacity){
    kprintf_raw("Has the manager been initialized");
    xhci_manager = new xHCIManager(capacity); 
    kprintf_raw("The manager has been initialized? %h",(uintptr_t)xhci_manager);
}

void xhci_configure_device(uint8_t slot_id, uint8_t endpoint_id, xhci_usb_device *device){
    xhci_manager->default_device = new xHCIKeyboard();
    xhci_manager->default_device->device = device;
    xhci_bridge_request_data(slot_id, endpoint_id);
}