#pragma once
#include "types.h"
#include "xhci_types.h"

class xHCIDevice {
public:
    virtual void request_data() = 0;

    virtual void process_data() = 0;

    xhci_usb_device *device;
};

class xHCIManager {
public:
    xHCIDevice *default_device;
};