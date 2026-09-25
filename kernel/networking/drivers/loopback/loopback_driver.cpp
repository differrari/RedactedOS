#include "loopback_driver.hpp"
#include "std/memory.h"

LoopbackDriver::LoopbackDriver(){
    rx_head = 0;
    rx_tail = 0;
    memset(rxq, 0, sizeof(rxq));
    hw_name[0]='l'; hw_name[1]='o'; hw_name[2]='o'; hw_name[3]='p'; hw_name[4]='b'; hw_name[5]='a'; hw_name[6]='c'; hw_name[7]='k'; hw_name[8]=0;
}

LoopbackDriver::~LoopbackDriver(){}

bool LoopbackDriver::init_at(uint64_t pci_addr, uint32_t irq_base_vector){
    (void)pci_addr;
    (void)irq_base_vector;
    return true;
}

netpkt_t* LoopbackDriver::handle_receive_packet(){
    if (rx_head == rx_tail) return nullptr;
    netpkt_t* p = rxq[rx_head];
    rxq[rx_head] = nullptr;
    rx_head = (uint16_t)((rx_head + 1) & 255);
    return p;
}

void LoopbackDriver::enable_verbose(){}

netdev_tx_result_t LoopbackDriver::send_packet(netpkt_t* packet){
    if (!packet || !netpkt_len(packet)) return NETDEV_TX_DROP;
    uint16_t next = (uint16_t)((rx_tail + 1) & 255);
    if (next == rx_head)return NETDEV_TX_BUSY;
    rxq[rx_tail] = packet;
    rx_tail = next;
    return NETDEV_TX_OK;
}

void LoopbackDriver::get_mac(uint8_t out_mac[6]) const{
    if (out_mac) memset(out_mac, 0, 6);
}

uint16_t LoopbackDriver::get_mtu() const{ return 65535; }

uint16_t LoopbackDriver::get_header_size() const{ return 0; }

const char* LoopbackDriver::hw_ifname() const{ return hw_name; }

uint32_t LoopbackDriver::get_speed_mbps() const{ return 0xFFFFFFFFu; }

uint8_t LoopbackDriver::get_duplex() const{ return 0xFFu; }