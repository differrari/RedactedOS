#pragma once

#include "types.h"
#include "std/indexmap.hpp"

class NetworkDispatch {
public:
    NetworkDispatch();
    bool bind_port(uint16_t port, uint16_t process);
    bool unbind_port(uint16_t port, uint16_t process);

private:
    IndexMap<uint16_t> ports;
};