#include "udp.h"
#include "console/kio.h"
#include "net/network_types.h"
#include "syscalls/syscalls.h"
#include "eth.h"
#include "ipv4.h"

void create_udp_packet(uintptr_t p, network_connection_ctx source, network_connection_ctx destination, sizedptr payload){
    p = create_eth_packet(p, source.mac, destination.mac, 0x800);

    p = create_ipv4_packet(p, sizeof(udp_hdr_t) + payload.size, 0x11, source.ip, destination.ip);

    udp_hdr_t* udp = (udp_hdr_t*)p;
    udp->src_port = __builtin_bswap16(source.port);
    udp->dst_port = __builtin_bswap16(destination.port);
    udp->length = __builtin_bswap16(sizeof(udp_hdr_t) + payload.size);

    p += sizeof(udp_hdr_t);

    uint8_t* data = (uint8_t*)p;
    uint8_t* payload_c = (uint8_t*)payload.ptr;
    for (size_t i = 0; i < payload.size; i++) data[i] = payload_c[i];

    udp->checksum = __builtin_bswap16(checksum16_pipv4(source.ip,destination.ip,0x11,(uint8_t*)udp,sizeof(udp_hdr_t) + payload.size));
    
}

uint16_t udp_parse_packet(uintptr_t ptr){
    udp_hdr_t* udp = (udp_hdr_t*)ptr;
    ptr += sizeof(udp_hdr_t);
    uint16_t port = __builtin_bswap16(udp->dst_port);
    return port;
}

sizedptr udp_parse_packet_payload(uintptr_t ptr){
    eth_hdr_t* eth = (eth_hdr_t*)ptr;
    
    ptr += sizeof(eth_hdr_t);
    
    if (__builtin_bswap16(eth->ethertype) == 0x800){
        ipv4_hdr_t* ip = (ipv4_hdr_t*)ptr;
        uint32_t srcip = __builtin_bswap32(ip->src_ip);
        ptr += sizeof(ipv4_hdr_t);
        if (ip->protocol == 0x11){
            udp_hdr_t* udp = (udp_hdr_t*)ptr;
            ptr += sizeof(udp_hdr_t);
            uint16_t payload_len = __builtin_bswap16(udp->length) - sizeof(udp_hdr_t);
            return (sizedptr){ptr,payload_len};
        }
    }

    return (sizedptr){0,0};
}