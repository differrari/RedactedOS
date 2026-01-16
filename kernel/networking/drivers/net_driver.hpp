#pragma once
#include "types.h"
#include "net/network_types.h"

class NetDriver {
public:
    NetDriver() = default;
    virtual ~NetDriver() = default;
    virtual bool init_at(uint64_t pci_addr, uint32_t irq_base_vector) = 0;
    virtual sizedptr allocate_packet(size_t size) = 0;
    virtual sizedptr handle_receive_packet() = 0;
    virtual void handle_sent_packet() = 0;
    virtual void enable_verbose() = 0;
    virtual bool send_packet(sizedptr packet) = 0;
    virtual void get_mac(uint8_t out_mac[6]) const = 0;
    virtual uint16_t get_mtu() const = 0;
    virtual uint16_t get_header_size() const = 0;
    virtual const char* hw_ifname() const = 0;
    virtual uint32_t get_speed_mbps() const = 0;
    virtual uint8_t get_duplex() const = 0;
    virtual bool sync_multicast(const uint8_t* macs, uint32_t count) {(void)macs; (void)count; return true; }
};