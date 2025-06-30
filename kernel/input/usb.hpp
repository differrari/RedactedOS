#pragma once

#include "types.h"
#include "keypress.h"
#include "xhci_types.h"

class USBDriver {
public:
    USBDriver() = default;
    virtual bool init() = 0;
    bool request_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t index, uint16_t wIndex, void *out_descriptor);
    virtual bool request_sized_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor) = 0;
    uint16_t packet_size(uint16_t speed);
    bool get_configuration(uint8_t address);
    void hub_enumerate(uint8_t address);
    bool port_reset(uint32_t *port);
    bool setup_device();
    virtual uint8_t address_device() = 0;
    bool poll_interrupt_in();
    virtual bool configure_endpoint(uint8_t address, usb_endpoint_descriptor *endpoint, uint8_t configuration_value) = 0;
    virtual void handle_hub_routing(uint8_t hub, uint8_t port) = 0;
    ~USBDriver() = default;
protected:
    void *mem_page;
};