#pragma once

#include "net/network_types.h"
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
    void send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len);
    bool read_packet(sizedptr *Packet, uint16_t process);

private:
    IndexMap<uint16_t> ports;
    NetDriver *driver;
};