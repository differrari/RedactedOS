#pragma once
#include "types.h"
#include "std/std.h"
#include "drivers/net_driver.hpp"
#include "net/network_types.h"
#include "networking/internet_layer/ipv4.h"
#include "interface_manager.h"
#include "data_struct/data_struct.hpp"

class NetworkDispatch {
public:
    NetworkDispatch();
    bool init();

    void handle_rx_irq(size_t nic_id);
    void handle_tx_irq(size_t nic_id);

    bool enqueue_frame(uint8_t ifindex, const sizedptr&);

    int net_task();
    void set_net_pid(uint16_t pid);
    uint16_t get_net_pid() const;

    size_t nic_count() const;

    const char* ifname(uint8_t ifindex) const;
    const char* hw_ifname(uint8_t ifindex) const;
    const uint8_t* mac(uint8_t ifindex) const;
    uint16_t mtu(uint8_t ifindex) const;
    uint16_t header_size(uint8_t ifindex) const;
    l2_interface_t* l2_at(uint8_t ifindex) const;
    NetDriver* driver_at(uint8_t ifindex) const;

    uint32_t speed(uint8_t ifindex) const;
    uint8_t duplex(uint8_t ifindex) const;
    uint8_t kind(uint8_t ifindex) const;

    void dump_interfaces();

private:
    struct NICCtx {
        NetDriver* drv;
        uint8_t ifindex;
        char ifname_str[16];
        char hwname_str[32];
        uint8_t mac_addr[6];
        uint16_t mtu_val;
        uint16_t hdr_sz;
        uint32_t speed_mbps;
        uint8_t duplex_mode;
        uint8_t kind_val;
        RingBuffer<sizedptr, 1024> tx;
        RingBuffer<sizedptr, 1024> rx;
        uint64_t rx_produced;
        uint64_t rx_consumed;
        uint64_t tx_produced;
        uint64_t tx_consumed;
        uint64_t rx_dropped;
        uint64_t tx_dropped;
    };

    static const size_t MAX_NIC = 16;

    NICCtx nics[MAX_NIC];
    size_t nic_num;
    uint16_t g_net_pid;

    uint8_t ifindex_to_nicid[MAX_L2_INTERFACES + 1];

    void free_frame(const sizedptr&);
    bool register_all_from_bus();
    void copy_str(char* dst, int cap, const char* src);

    int nic_for_ifindex(uint8_t ifindex) const;
};
