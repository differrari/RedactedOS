#include "USBManager.hpp"
#include "USBKeyboard.hpp"
#include "console/kio.h"

USBManager::USBManager(uint32_t capacity){
    devices = IndexMap<USBDevice*>(capacity);
};

void USBManager::register_device(uint8_t slot_id, xhci_usb_device *device){
    if (slot_id >= devices.max_size()) return;
    USBDevice *newdevice = new USBDevice(8,device);
    if (!newdevice) return;
    devices.add(slot_id,newdevice);
}

void USBManager::register_endpoint(uint8_t slot_id, xhci_usb_device_endpoint *endpoint){
    if (slot_id >= devices.max_size()) return;
    devices[slot_id]->register_endpoint(endpoint);
}

void USBManager::request_data(uint8_t slot_id, uint8_t endpoint_id){
    if (slot_id >= devices.max_size()) return;//TODO: check if it exists
    devices[slot_id]->request_data(endpoint_id);
}

void USBManager::process_data(uint8_t slot_id, uint8_t endpoint_id){
    if (slot_id >= devices.max_size()) return;//TODO: check if it exists
    devices[slot_id]->process_data(endpoint_id);
}
