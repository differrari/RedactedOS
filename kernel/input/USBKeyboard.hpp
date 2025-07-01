#pragma once

#include "types.h"
#include "USBDevice.hpp"
#include "keypress.h"

class USBKeyboard: public USBEndpoint {
public:
    USBKeyboard(uint8_t new_slot_id, xhci_usb_device_endpoint *endpoint) : USBEndpoint(endpoint), slot_id(new_slot_id) {}
    void request_data() override;
    void process_data() override;
private:
    trb* latest_ring;
    bool requesting = false;
    uint8_t slot_id;

    keypress last_keypress;

    int repeated_keypresses = 0; 
};