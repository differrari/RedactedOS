#pragma once
#include "types.h"
#include "xhci_types.h"
#include "std/std.hpp"
#include "USBDevice.hpp"

class USBManager {
public:
    USBManager(uint32_t capacity);
    IndexMap<USBDevice*> devices;
    USBDevice *default_device;

    void register_device(uint8_t slot_id, xhci_usb_device *device);
    void register_endpoint(uint8_t slot_id, xhci_usb_device_endpoint *endpoint);
    void request_data(uint8_t slot_id, uint8_t endpoint_id);
    void process_data(uint8_t slot_id, uint8_t endpoint_id);
};