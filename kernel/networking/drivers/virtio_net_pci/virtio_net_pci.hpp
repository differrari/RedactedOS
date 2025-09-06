#pragma once
#include "../net_driver.hpp"
#include "virtio/virtio_pci.h"
#include "net/network_types.h"

class VirtioNetDriver : public NetDriver {
public:
    VirtioNetDriver();
    bool init_at(uint64_t pci_addr, uint32_t irq_base_vector) override;
    sizedptr allocate_packet(size_t size) override;
    sizedptr handle_receive_packet() override;
    void handle_sent_packet() override;
    void enable_verbose() override;
    void send_packet(sizedptr packet) override;
    void get_mac(uint8_t out_mac[6]) const override;
    uint16_t get_mtu() const override;
    uint16_t get_header_size() const override;
    const char* if_prefix() const override;
    const char* hw_ifname() const override;
    ~VirtioNetDriver() override;

private:
    bool verbose;
    uint16_t last_used_receive_idx;
    uint16_t last_used_sent_idx;
    virtio_device vnp_net_dev;
    uint16_t header_size;
    uint16_t mtu;
    char hw_name[32];
};