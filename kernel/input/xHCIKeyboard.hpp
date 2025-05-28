#pragma once

#include "types.h"
#include "xHCIManager.hpp"
#include "keypress.h"

class xHCIKeyboard: public xHCIDevice {
public:
    xHCIKeyboard(xhci_usb_device *dev) : xHCIDevice(dev) {}
    void request_data(uint8_t endpoint_id) override;
    void process_data(uint8_t endpoint_id) override;
private:
    trb* latest_ring;
    bool requesting = false;

    keypress last_keypress;

    int repeated_keypresses = 0; 
};