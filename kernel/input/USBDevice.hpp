#pragma once
#include "types.h"
#include "usb_types.h"
#include "std/std.h"
#include "console/kio.h"

class USBDriver;

class USBEndpoint {
public:
    USBEndpoint(uint8_t endpoint, usb_device_types type, uint32_t interval, uint16_t packet_size): endpoint(endpoint), type(type), packet_size(packet_size), interval(interval) { }
    virtual void request_data(USBDriver *driver) = 0;

    virtual void process_data(USBDriver *driver) = 0;

    uint8_t endpoint;
    usb_device_types type;
    uint16_t packet_size;
    uint32_t interval;
};

class USBDevice {
public:
    USBDevice(uint32_t capacity, uint8_t address);
    void request_data(uint8_t endpoint_id, USBDriver *driver);

    void process_data(uint8_t endpoint_id, USBDriver *driver);

    void register_endpoint(uint8_t endpoint, usb_device_types type, uint32_t interval, uint16_t packet_size);

    void poll_inputs(USBDriver *driver);

    IndexMap<USBEndpoint*> endpoints;
    uint8_t address;
};