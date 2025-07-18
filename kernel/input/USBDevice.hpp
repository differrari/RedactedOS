#pragma once
#include "types.h"
#include "usb_types.h"
#include "std/std.hpp"
#include "console/kio.h"

class USBDriver;

class USBEndpoint {
public:
    USBEndpoint(uint8_t endpoint, xhci_device_types type, uint16_t packet_size): endpoint(endpoint), type(type), packet_size(packet_size) { }
    virtual void request_data(USBDriver *driver) = 0;

    virtual void process_data(USBDriver *driver) = 0;

    uint8_t endpoint;
    xhci_device_types type;
    uint16_t packet_size;
};

class USBDevice {
public:
    USBDevice(uint32_t capacity, uint8_t address);
    void request_data(uint8_t endpoint_id, USBDriver *driver);

    void process_data(uint8_t endpoint_id, USBDriver *driver);

    void register_endpoint(uint8_t endpoint, xhci_device_types type, uint16_t packet_size);

    void poll_inputs(USBDriver *driver);

    IndexMap<USBEndpoint*> endpoints;
    uint8_t address;
};