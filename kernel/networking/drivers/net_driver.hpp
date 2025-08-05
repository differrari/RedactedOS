#pragma once

#include "types.h"
#include "net/network_types.h" 

class NetDriver {
public:
    NetDriver() = default;
    virtual bool init() = 0;

    virtual sizedptr allocate_packet(size_t size) = 0;
    virtual sizedptr handle_receive_packet(void* buffer) = 0;
    virtual void handle_sent_packet() = 0;
    virtual void enable_verbose() = 0;
    virtual void send_packet(sizedptr packet) = 0;
    virtual void get_mac(net_l2l3_endpoint *context) = 0;

    virtual ~NetDriver() = default;

    uint16_t header_size;
};
