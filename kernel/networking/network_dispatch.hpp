#pragma once

#include "types.h"
#include "std/indexmap.hpp"
#include "drivers/net_driver.hpp"

class NetworkDispatch {
public:
    NetworkDispatch();
    bool init();
    bool bind_port(uint16_t port, uint16_t process);
    bool unbind_port(uint16_t port, uint16_t process);
    void handle_interrupt();

private:
    IndexMap<uint16_t> ports;
    NetDriver *driver;
};