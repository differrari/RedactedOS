#pragma once
#include "types.h"
#include "networking/drivers/net_driver.hpp"
#include "virtio/virtio_pci.h"
#include "std/memory.h"
#include "net/link_layer/nic_types.h"

typedef struct __attribute__((packed)) virtio_net_hdr_t {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} virtio_net_hdr_t;

typedef struct __attribute__((packed)) virtio_net_config {
    uint8_t mac[6];
    uint16_t status;
    uint16_t max_virtqueue_pairs;
    uint16_t mtu;
    uint32_t speed;
    uint8_t duplex;
    uint8_t rss_max_key_size;
    uint16_t rss_max_indirection_table_length;
    uint32_t supported_hash_types;
} virtio_net_config;

class VirtioNetDriver : public NetDriver {
public:
    VirtioNetDriver();
    ~VirtioNetDriver();

    bool init_at(uint64_t pci_addr, uint32_t irq_base_vector);
    void get_mac(uint8_t out_mac[6]) const override;
    uint16_t get_mtu() const override;
    uint16_t get_header_size() const override;
    const char* hw_ifname() const override;
    void enable_verbose() override;

    uint32_t get_speed_mbps() const override;
    uint8_t get_duplex() const override;

    sizedptr allocate_packet(size_t size) override;
    sizedptr handle_receive_packet() override;
    void handle_sent_packet() override;
    bool send_packet(sizedptr packet) override;

private:
    virtio_device vnp_net_dev;

    bool verbose;
    uint16_t header_size;
    uint16_t mtu;
    uint32_t speed_mbps;
    LinkDuplex duplex;
    char hw_name[8];

    uint16_t last_used_receive_idx;
    uint16_t last_used_sent_idx;
};
