#pragma once

#include "types.h"
#include "std/string.h"
#include "ui/graphic_types.h"

class NetDriver {
public:
    NetDriver(){}
    virtual bool init() = 0;

    virtual void handle_interrupt() = 0;

    virtual void send_packet(void* packet, size_t size) = 0;

    virtual void enable_verbose() = 0;

    virtual ~NetDriver() = default;
};