#include "xhci_bridge.h"
#include "memory/kalloc.h"
#include "console/kio.h"
#include "input_dispatch.h"
#include "memory/page_allocator.h"
#include "xHCIManager.hpp"
#include "xHCIKeyboard.hpp"

xHCIManager *xhci_manager;

void xhci_bridge_request_data(uint8_t slot_id, uint8_t endpoint_id) {
    xhci_manager->request_data(slot_id,endpoint_id);
}

void xhci_process_input(uint8_t slot_id, uint8_t endpoint_id){
    xhci_manager->process_data(slot_id,endpoint_id);
}

void xhci_initialize_manager(uint32_t capacity){
    xhci_manager = new xHCIManager(capacity); 
}

void xhci_configure_device(uint8_t slot_id, uint8_t endpoint_id, xhci_usb_device *device){
    xhci_manager->register_device(slot_id,endpoint_id,device);
}