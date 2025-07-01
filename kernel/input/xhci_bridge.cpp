#include "xhci_bridge.h"
#include "memory/kalloc.h"
#include "input_dispatch.h"
#include "memory/page_allocator.h"
#include "USBManager.hpp"
#include "USBKeyboard.hpp"
#include "xhci_types.h"

USBManager *xhci_manager;

void xhci_bridge_request_data(uint8_t slot_id, uint8_t endpoint_id) {
    // xhci_manager->request_data(slot_id,endpoint_id);
}

void xhci_process_input(uint8_t slot_id, uint8_t endpoint_id){
    // xhci_manager->process_data(slot_id,endpoint_id);
}

void xhci_initialize_manager(uint32_t capacity){
    // xhci_manager = new USBManager(capacity); 
}

void xhci_configure_device(uint8_t slot_id){
    // xhci_manager->register_device(slot_id);
}

void xhci_configure_endpoint(uint8_t slot_id, uint8_t endpoint, xhci_device_types type, uint16_t packet_size){
    // xhci_manager->register_endpoint(slot_id, endpoint, type, packet_size);
}