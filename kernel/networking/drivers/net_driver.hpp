#pragma once

#include "types.h"
#include "net/network_types.h" 

class NetDriver {
public:
    NetDriver() = default;
    virtual bool init() = 0;

    virtual sizedptr allocate_packet(size_t size) = 0;
    virtual sizedptr handle_receive_packet() = 0;
    virtual void handle_sent_packet() = 0;
    virtual void enable_verbose() = 0;
    virtual void send_packet(sizedptr packet) = 0;
    virtual void get_mac(uint8_t out_mac[6]) = 0;

    virtual ~NetDriver() = default;

    uint16_t header_size;
};
