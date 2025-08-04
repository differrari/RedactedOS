#pragma once
#include "types.h"
#include "usb_types.h"
#include "std/std.hpp"
#include "USBDevice.hpp"

class USBDriver;

class USBManager {
public:
    USBManager(uint32_t capacity);
    IndexMap<USBDevice*> devices;

    void register_device(uint8_t address);
    void register_endpoint(uint8_t slot_id, uint8_t endpoint, usb_device_types type, uint16_t packet_size);
    void request_data(uint8_t slot_id, uint8_t endpoint_id, USBDriver *driver);
    void process_data(uint8_t slot_id, uint8_t endpoint_id, USBDriver *driver);
    void poll_inputs(USBDriver *driver);
};