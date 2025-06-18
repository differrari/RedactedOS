#pragma once

#include "types.h"
#include "std/string.h"
#include "ui/graphic_types.h"
#include "networking/packets.h"

class NetDriver {
public:
    NetDriver(){}
    virtual bool init() = 0;

    virtual void handle_interrupt() = 0;

    virtual void enable_verbose() = 0;

    virtual void send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len) = 0;

    virtual ~NetDriver() = default;
};