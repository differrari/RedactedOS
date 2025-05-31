#include "xHCIDevice.hpp"
#include "xHCIKeyboard.hpp"

xHCIDevice::xHCIDevice(uint32_t capacity, xhci_usb_device *dev) : device(dev) {
    endpoints = IndexMap<xHCIEndpoint*>(capacity);
};

void xHCIDevice::request_data(uint8_t endpoint_id){
    if (endpoint_id >= endpoints.max_size()) return;//TODO: check if it exists
    endpoints[endpoint_id]->request_data();
}

void xHCIDevice::process_data(uint8_t endpoint_id){
    if (endpoint_id >= endpoints.max_size()) return;//TODO: check if it exists
    endpoints[endpoint_id]->process_data();
}

void xHCIDevice::register_endpoint(xhci_usb_device_endpoint *endpoint){
    if (endpoint->poll_endpoint >= endpoints.max_size()) return;
    xHCIEndpoint *newendpoint;
    switch (endpoint->type){
        case KEYBOARD:
            newendpoint = new xHCIKeyboard(device->slot_id, endpoint);
            break;
        default: return;
    }
    if (!newendpoint) return;
    endpoints.add(endpoint->poll_endpoint,newendpoint);
    request_data(endpoint->poll_endpoint);
}