#pragma once

#include "usb.hpp"
#include "usb_types.h"

typedef struct xhci_ring {
    trb* ring;
    uint8_t cycle_bit;
    uint32_t index;
} xhci_ring;

class XHCIDriver : public USBDriver {
public:
    XHCIDriver() = default;
    bool init() override;
    bool request_sized_descriptor(uint8_t address, uint8_t endpoint, uint8_t rType, uint8_t request, uint8_t type, uint16_t descriptor_index, uint16_t wIndex, uint16_t descriptor_size, void *out_descriptor) override;
    uint8_t address_device(uint8_t address) override;
    bool configure_endpoint(uint8_t address, usb_endpoint_descriptor *endpoint, uint8_t configuration_value, xhci_device_types type) override;
    void handle_hub_routing(uint8_t hub, uint8_t port) override;
    bool poll(uint8_t address, uint8_t endpoint, void *out_buf, uint16_t size) override;
    bool setup_device(uint8_t address, uint16_t port) override;
    void handle_interrupt() override;
    ~XHCIDriver() = default;
private:
    bool check_fatal_error();
    bool enable_events();

    bool port_reset(uint16_t port);

    bool issue_command(uint64_t param, uint32_t status, uint32_t control);
    void ring_doorbell(uint32_t slot, uint32_t endpoint);
    bool await_response(uint64_t command, uint32_t type);
    uint8_t get_ep_type(usb_endpoint_descriptor* descriptor);

    void make_ring_link_control(trb* ring, bool cycle);
    void make_ring_link(trb* ring, bool cycle);

    xhci_cap_regs* cap;
    xhci_op_regs* op;
    xhci_port_regs* ports;
    xhci_interrupter* interrupter;
    uintptr_t db_base;
    uintptr_t rt_base;
    uintptr_t* dcbaap;
    uint16_t max_device_slots;
    uint16_t max_ports;

    xhci_ring command_ring;
    xhci_ring event_ring;
    
    trb* last_event;

    IndexMap<xhci_ring> endpoint_map;
    IndexMap<xhci_input_context*> context_map;
};