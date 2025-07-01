#include "USBManager.hpp"
#include "USBDevice.hpp"
#include "USBKeyboard.hpp"
#include "console/kio.h"

USBManager::USBManager(uint32_t capacity){
    devices = IndexMap<USBDevice*>(capacity);
};

void USBManager::register_device(uint8_t address){
    if (address >= devices.max_size()) return;
    USBDevice *newdevice = new USBDevice(8,address);
    if (!newdevice) return;
    devices.add(address,newdevice);
}

void USBManager::register_endpoint(uint8_t slot_id, uint8_t endpoint, xhci_device_types type, uint16_t packet_size){
    if (slot_id >= devices.max_size()) return;
    USBDevice *dev = devices[slot_id];
    if (dev)
        dev->register_endpoint(endpoint, type, packet_size);
}

void USBManager::request_data(uint8_t slot_id, uint8_t endpoint_id, USBDriver *driver){
    if (slot_id >= devices.max_size()) return;//TODO: check if it exists
    USBDevice *dev = devices[slot_id];
    if (dev)
        dev->request_data(endpoint_id, driver);
}

void USBManager::process_data(uint8_t slot_id, uint8_t endpoint_id, USBDriver *driver){
    if (slot_id >= devices.max_size()) return;//TODO: check if it exists
    USBDevice *dev = devices[slot_id];
    if (dev)
        dev->process_data(endpoint_id, driver);
}

void USBManager::poll_inputs(USBDriver *driver){
    for (uint8_t i = 0; i < devices.max_size(); i++){
        USBDevice *device = devices[i];
        if (device){
            device->poll_inputs(driver);
        }
    }
}