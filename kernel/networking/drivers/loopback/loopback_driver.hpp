#pragma once

#include "networking/drivers/net_driver.hpp"
#include "networking/link_layer/nic_types.h"

class LoopbackDriver : public NetDriver {
public:
    LoopbackDriver();
    ~LoopbackDriver() override;

    bool init_at(uint64_t pci_addr, uint32_t irq_base_vector) override;
    netpkt_t* handle_receive_packet() override;
    void enable_verbose() override;
    bool send_packet(netpkt_t* packet) override;
    void get_mac(uint8_t out_mac[6]) const override;
    uint16_t get_mtu() const override;
    uint16_t get_header_size() const override;
    const char* hw_ifname() const override;
    uint32_t get_speed_mbps() const override;
    uint8_t get_duplex() const override;

private:
    netpkt_t* rxq[256];
    uint16_t rx_head;
    uint16_t rx_tail;
    char hw_name[16];
};