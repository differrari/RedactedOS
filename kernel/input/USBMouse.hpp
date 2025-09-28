#pragma once

#include "types.h"
#include "USBDevice.hpp"
#include "mouse_input.h"
#include "usb_types.h"

class USBMouse: public USBEndpoint {
public:
    USBMouse(uint8_t new_slot_id, uint8_t endpoint, uint32_t interval, uint16_t packet_size) : USBEndpoint(endpoint, MOUSE, interval, packet_size), slot_id(new_slot_id) {}
    void request_data(USBDriver *driver) override;
    void process_data(USBDriver *driver) override;
private:
    void process_mouse_input(mouse_input *rkp);
    bool requesting = false;
    uint8_t slot_id;

    int repeated_keypresses = 0; 

    void* buffer;
};