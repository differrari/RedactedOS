#include "USBDevice.hpp"
#include "USBKeyboard.hpp"
#include "usb_types.h"
#include "console/kio.h"

USBDevice::USBDevice(uint32_t capacity, uint8_t address) : address(address) {
    endpoints = IndexMap<USBEndpoint*>(capacity);
};

void USBDevice::request_data(uint8_t endpoint_id, USBDriver *driver){
    if (endpoint_id >= endpoints.max_size()) return;//TODO: check if it exists
    USBEndpoint *ep = endpoints[endpoint_id];
    if (ep)
        ep->request_data(driver);
}

void USBDevice::process_data(uint8_t endpoint_id, USBDriver *driver){
    if (endpoint_id >= endpoints.max_size()) return;//TODO: check if it exists
    USBEndpoint *ep = endpoints[endpoint_id];
    if (ep)
        ep->process_data(driver);
}

void USBDevice::register_endpoint(uint8_t endpoint, usb_device_types type, uint16_t packet_size){
    if (endpoint >= endpoints.max_size()) return;
    USBEndpoint *newendpoint;
    switch (type){
        case KEYBOARD:
            newendpoint = new USBKeyboard(address, endpoint, packet_size);
            break;
        default: return;
    }
    if (!newendpoint) return;
    endpoints.add(endpoint,newendpoint);
}

void USBDevice::poll_inputs(USBDriver *driver){
    for (uint8_t i = 0; i < endpoints.max_size(); i++){
        USBEndpoint *ep = endpoints[i];
        if (ep && ep->type == KEYBOARD)
            ep->request_data(driver);
    }
}