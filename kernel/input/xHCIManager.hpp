#pragma once
#include "types.h"
#include "xhci_types.h"
#include "std/std.hpp"

class xHCIDevice {
public:
    virtual void request_data(uint8_t endpoint_id) = 0;

    virtual void process_data(uint8_t endpoint_id) = 0;

    xhci_usb_device *device;
};

class xHCIDummy: public xHCIDevice {
    void request_data(uint8_t endpoint_id) override {}
    void process_data(uint8_t endpoint_id) override {}
};

class xHCIManager {
public:
    xHCIManager(uint32_t capacity);
    xHCIDevice *dummy_device;
    IndexMap<xHCIDevice*> devices;
    xHCIDevice *default_device;

    void register_device(uint8_t slot_id, uint8_t endpoint_id, xhci_usb_device *device);
    void request_data(uint8_t slot_id, uint8_t endpoint_id);
    void process_data(uint8_t slot_id, uint8_t endpoint_id);
};