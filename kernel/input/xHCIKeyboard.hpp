#pragma once
#include "types.h"
#include "xHCIManager.hpp"

class xHCIKeyboard: public xHCIDevice {
public:
    void request_data() override;
    void process_data() override;
};