#include "eth.h"
#include "arp.h"
#include "std/memory.h"
#include "net/internet_layer/ipv4.h"
#include "networking/network.h"

extern int net_tx_frame(uintptr_t frame_ptr, uint32_t frame_len);
extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);

uintptr_t create_eth_packet(uintptr_t p,
                            const uint8_t src_mac[6],
                            const uint8_t dst_mac[6],
                            uint16_t type)
{
    eth_hdr_t* eth =(eth_hdr_t*)p;
    memcpy(eth->src_mac, src_mac, 6);
    memcpy(eth->dst_mac, dst_mac, 6);
    eth->ethertype = __builtin_bswap16(type);
    return p + sizeof(eth_hdr_t);
}

uint16_t eth_parse_packet_type(uintptr_t ptr) {
    const eth_hdr_t* eth = (const eth_hdr_t*)ptr;
    return __builtin_bswap16(eth->ethertype);
}

const uint8_t* eth_get_source(uintptr_t ptr){
    const eth_hdr_t* eth = (const eth_hdr_t*)ptr;
    return eth->src_mac;
}

bool eth_send_frame(uint16_t ethertype,
                    const uint8_t dst_mac[6],
                    sizedptr payload)
{
    const uint8_t* src_mac = network_get_local_mac();

    uint32_t total = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)payload.size;
    uintptr_t buf = (uintptr_t)malloc(total);
    if (!buf) return false;

    uintptr_t ptr = create_eth_packet(buf, src_mac, dst_mac, ethertype);
    if (payload.size) {
        memcpy((void*)ptr, (void*)payload.ptr, payload.size);
    }

    bool ok = net_tx_frame(buf, total);

    free((void*)buf, total);
    return ok;
}

void eth_input(uintptr_t frame_ptr, uint32_t frame_len) {
    if (frame_len < sizeof(eth_hdr_t)) return;

    uint16_t type = eth_parse_packet_type(frame_ptr);
    const uint8_t* src_mac = eth_get_source(frame_ptr);
    uintptr_t payload_ptr = frame_ptr + sizeof(eth_hdr_t);
    uint32_t payload_len = frame_len - sizeof(eth_hdr_t);

    switch (type) {
    case 0x0806:
        arp_input(frame_ptr, frame_len);
        break;
    case 0x0800:
        ip_input(payload_ptr, payload_len, src_mac);
        break;
    default:
        break;
    }
}
