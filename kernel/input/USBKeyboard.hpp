#pragma once

#include "types.h"
#include "USBDevice.hpp"
#include "keypress.h"
#include "usb_types.h"

class USBKeyboard: public USBEndpoint {
public:
    USBKeyboard(uint8_t new_slot_id, uint8_t endpoint, uint16_t packet_size) : USBEndpoint(endpoint, KEYBOARD, packet_size), slot_id(new_slot_id) {}
    void request_data(USBDriver *driver) override;
    void process_data(USBDriver *driver) override;
private:
    void process_keypress(keypress *rkp);
    trb* latest_ring;
    bool requesting = false;
    uint8_t slot_id;

    keypress last_keypress;

    int repeated_keypresses = 0; 

    void* buffer;
};