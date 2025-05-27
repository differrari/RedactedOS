#pragma once

#include "types.h"
#include "xHCIManager.hpp"
#include "keypress.h"

class xHCIKeyboard: public xHCIDevice {
public:
    void request_data() override;
    void process_data() override;
private:
    trb* latest_ring;
    bool requesting = false;

    keypress last_keypress;

    int repeated_keypresses = 0; 
};