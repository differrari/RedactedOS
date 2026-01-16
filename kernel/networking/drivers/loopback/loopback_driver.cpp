#include "loopback_driver.hpp"
#include "std/memory.h"
#include "memory/page_allocator.h"

LoopbackDriver::LoopbackDriver(){
    memory_page = 0;
    rx_head = 0;
    rx_tail = 0;
    verbose = false;
    hw_name[0]='l'; hw_name[1]='o'; hw_name[2]='o'; hw_name[3]='p'; hw_name[4]='b'; hw_name[5]='a'; hw_name[6]='c'; hw_name[7]='k'; hw_name[8]=0;
}

LoopbackDriver::~LoopbackDriver(){}

bool LoopbackDriver::init_at(uint64_t pci_addr, uint32_t irq_base_vector){
    (void)pci_addr;
    (void)irq_base_vector;
    if (!memory_page) {
        memory_page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, true);
        if (!memory_page) return false;
    }
    return true;
}

sizedptr LoopbackDriver::allocate_packet(size_t size){
    if (!size) return (sizedptr){0,0};
    if (!memory_page && !init_at(0, 0)) return (sizedptr){0,0};
    void* p = kalloc(memory_page, size, ALIGN_16B, MEM_PRIV_KERNEL);
    return (sizedptr){(uintptr_t)p, (uint32_t)size};
}

sizedptr LoopbackDriver::handle_receive_packet(){
    if (rx_head == rx_tail) return (sizedptr){0,0};
    sizedptr p = rxq[rx_head];
    rx_head = (uint16_t)((rx_head + 1) & 255);
    return p;
}

void LoopbackDriver::handle_sent_packet(){}

void LoopbackDriver::enable_verbose(){ verbose = true; }

bool LoopbackDriver::send_packet(sizedptr packet){
    if (!packet.ptr || !packet.size) return false;
    uint16_t next = (uint16_t)((rx_tail + 1) & 255);
    if (next == rx_head)return false;
    rxq[rx_tail] = packet;
    rx_tail = next;
    return true;
}

void LoopbackDriver::get_mac(uint8_t out_mac[6]) const{
    if (out_mac) memset(out_mac, 0, 6);
}

uint16_t LoopbackDriver::get_mtu() const{ return 65535; }

uint16_t LoopbackDriver::get_header_size() const{ return 0; }

const char* LoopbackDriver::hw_ifname() const{ return hw_name; }

uint32_t LoopbackDriver::get_speed_mbps() const{ return 0xFFFFFFFFu; }

uint8_t LoopbackDriver::get_duplex() const{ return 0xFFu; }