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
    void handle_upload_interrupt();
    void handle_download_interrupt();
    //TODO: use sizedptr
    void send_packet(NetProtocol protocol, uint16_t port, network_connection_ctx *destination, void* payload, uint16_t payload_len);
    bool read_packet(sizedptr *Packet, uint16_t process);

    network_connection_ctx* get_context();

private:
    IndexMap<uint16_t> ports;
    NetDriver *driver;

    NetDriver* select_driver();

    sizedptr allocate_packet(size_t size);
    network_connection_ctx context;
};