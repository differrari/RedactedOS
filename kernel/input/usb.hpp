#pragma once

#include "types.h"
#include "keypress.h"
#include "usb_types.h"
#include "USBManager.hpp"

class USBDriver {
public:
    USBDriver() = default;
    virtual bool init() = 0;
    bool request_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t index, uint16_t wIndex, void *out_descriptor);
    virtual bool request_sized_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor) = 0;
    uint16_t packet_size(uint16_t speed);
    bool get_configuration(uint8_t address);
    void hub_enumerate(uint8_t address);
    virtual bool setup_device(uint8_t address, uint16_t port);
    virtual uint8_t address_device(uint8_t address) = 0;
    virtual bool configure_endpoint(uint8_t address, usb_endpoint_descriptor *endpoint, uint8_t configuration_value, xhci_device_types type) = 0;
    virtual void handle_hub_routing(uint8_t hub, uint8_t port) = 0;
    virtual bool poll(uint8_t address, uint8_t endpoint, void *out_buf, uint16_t size) = 0;
    void poll_inputs();
    virtual void handle_interrupt() = 0;
    bool use_interrupts;
    ~USBDriver() = default;
protected:
    void *mem_page;
    USBManager *usb_manager;
    bool verbose = false;
};