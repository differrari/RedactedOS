#pragma once

#include "types.h"
#include "USBDevice.hpp"
#include "keyboard_input.h"
#include "usb_types.h"

class USBKeyboard: public USBEndpoint {
public:
    USBKeyboard(uint8_t new_slot_id, uint8_t endpoint, uint32_t interval, uint16_t packet_size) : USBEndpoint(endpoint, KEYBOARD, interval, packet_size), slot_id(new_slot_id) {}
    void request_data(USBDriver *driver) override;
    void process_data(USBDriver *driver) override;
private:
    void process_keypress(keypress *rkp);
    bool requesting = false;
    uint8_t slot_id;

    __attribute__((aligned(16))) keypress last_keypress = {};

    int repeated_keypresses = 0; 

    void* buffer;
};