#include "USBDevice.hpp"
#include "USBKeyboard.hpp"

USBDevice::USBDevice(uint32_t capacity, xhci_usb_device *dev) : device(dev) {
    endpoints = IndexMap<USBEndpoint*>(capacity);
};

void USBDevice::request_data(uint8_t endpoint_id){
    if (endpoint_id >= endpoints.max_size()) return;//TODO: check if it exists
    endpoints[endpoint_id]->request_data();
}

void USBDevice::process_data(uint8_t endpoint_id){
    if (endpoint_id >= endpoints.max_size()) return;//TODO: check if it exists
    endpoints[endpoint_id]->process_data();
}

void USBDevice::register_endpoint(xhci_usb_device_endpoint *endpoint){
    if (endpoint->poll_endpoint >= endpoints.max_size()) return;
    USBEndpoint *newendpoint;
    switch (endpoint->type){
        case KEYBOARD:
            newendpoint = new USBKeyboard(device->slot_id, endpoint);
            break;
        default: return;
    }
    if (!newendpoint) return;
    endpoints.add(endpoint->poll_endpoint,newendpoint);
    request_data(endpoint->poll_endpoint);
}