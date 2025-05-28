#include "xHCIManager.hpp"
#include "xHCIKeyboard.hpp"
#include "console/kio.h"

xHCIManager::xHCIManager(uint32_t capacity){
    devices = IndexMap<xHCIDevice*>(capacity);
    dummy_device = new xHCIDevice(0,nullptr);//TODO: Our allocator fails if we allocate only one thing in a constructor, so we allocate a dummy here
};

void xHCIManager::register_device(uint8_t slot_id, xhci_usb_device *device){
    if (slot_id >= devices.max_size()) return;
    xHCIDevice *newdevice = new xHCIDevice(8,device);
    if (!newdevice) return;
    devices.add(slot_id,newdevice);
}

void xHCIManager::register_endpoint(uint8_t slot_id, xhci_usb_device_endpoint *endpoint){
    if (slot_id >= devices.max_size()) return;
    devices[slot_id]->register_endpoint(endpoint);
}

void xHCIManager::request_data(uint8_t slot_id, uint8_t endpoint_id){
    if (slot_id >= devices.max_size()) return;//TODO: check if it exists
    devices[slot_id]->request_data(endpoint_id);
}

void xHCIManager::process_data(uint8_t slot_id, uint8_t endpoint_id){
    if (slot_id >= devices.max_size()) return;//TODO: check if it exists
    devices[slot_id]->process_data(endpoint_id);
}
