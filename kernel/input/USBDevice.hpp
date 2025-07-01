#pragma once
#include "types.h"
#include "xhci_types.h"
#include "std/std.hpp"

class USBEndpoint {
public:
    USBEndpoint(xhci_usb_device_endpoint *ep): endpoint(ep) {}
    virtual void request_data() {};

    virtual void process_data() {};

    xhci_usb_device_endpoint *endpoint;
};

class USBDevice {
public:
    USBDevice(uint32_t capacity, xhci_usb_device *dev);
    void request_data(uint8_t endpoint_id);

    void process_data(uint8_t endpoint_id);

    void register_endpoint(xhci_usb_device_endpoint *endpoint);

    IndexMap<USBEndpoint*> endpoints;
    xhci_usb_device *device;
};