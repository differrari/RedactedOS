#include "udp.h"
#include "console/kio.h"
#include "net/network_types.h"
#include "syscalls/syscalls.h"

uint16_t udp_checksum(
    uint32_t src_ip,
    uint32_t dst_ip,
    uint8_t protocol,
    const uint8_t* udp_header_and_payload,
    uint16_t length
) {
    uint32_t sum = 0;

    sum += (src_ip >> 16) & 0xFFFF;
    sum += src_ip & 0xFFFF;
    sum += (dst_ip >> 16) & 0xFFFF;
    sum += dst_ip & 0xFFFF;
    sum += protocol;
    sum += length;

    for (uint16_t i = 0; i + 1 < length; i += 2)
        sum += (udp_header_and_payload[i] << 8) | udp_header_and_payload[i + 1];

    if (length & 1)
        sum += udp_header_and_payload[length - 1] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return ~sum;
}

void create_udp_packet(uint8_t* buf, network_connection_ctx source, network_connection_ctx destination, const uint8_t* payload, uint16_t payload_len) {
    uintptr_t p = (uintptr_t)buf;

    eth_hdr_t* eth = (eth_hdr_t*)p;
    for (int i = 0; i < 6; i++) eth->dst_mac[i] = destination.mac[i];
    for (int i = 0; i < 6; i++) eth->src_mac[i] = source.mac[i];
    eth->ethertype = __builtin_bswap16(0x0800);
    p += sizeof(eth_hdr_t);

    ipv4_hdr_t* ip = (ipv4_hdr_t*)p;
    ip->version_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_length = __builtin_bswap16(sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + payload_len);
    ip->identification = 0;
    ip->flags_frag_offset = __builtin_bswap16(0x4000);
    ip->ttl = 64;
    ip->protocol = 0x11;
    ip->header_checksum = 0;
    ip->src_ip = __builtin_bswap32(source.ip);
    ip->dst_ip = __builtin_bswap32(destination.ip);
    uint16_t* ip_words = (uint16_t*)ip;
    uint32_t sum = 0;
    for (int i = 0; i < 10; i++) sum += ip_words[i];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    ip->header_checksum = ~sum;
    p += sizeof(ipv4_hdr_t);

    udp_hdr_t* udp = (udp_hdr_t*)p;
    udp->src_port = __builtin_bswap16(source.port);
    udp->dst_port = __builtin_bswap16(destination.port);
    udp->length = __builtin_bswap16(sizeof(udp_hdr_t) + payload_len);

    p += sizeof(udp_hdr_t);

    uint8_t* data = (uint8_t*)p;
    for (int i = 0; i < payload_len; i++) data[i] = payload[i];

    udp->checksum = __builtin_bswap16(udp_checksum(source.ip,destination.ip,ip->protocol,(uint8_t*)udp,sizeof(udp_hdr_t) + payload_len));
    
}

uint16_t udp_parse_packet(uintptr_t ptr){
    eth_hdr_t* eth = (eth_hdr_t*)ptr;
    
    ptr += sizeof(eth_hdr_t);
    
    if (__builtin_bswap16(eth->ethertype) == 0x800){
        ipv4_hdr_t* ip = (ipv4_hdr_t*)ptr;
        uint32_t srcip = __builtin_bswap32(ip->src_ip);
        ptr += sizeof(ipv4_hdr_t);
        if (ip->protocol == 0x11){
            udp_hdr_t* udp = (udp_hdr_t*)ptr;
            ptr += sizeof(udp_hdr_t);
            uint16_t port = __builtin_bswap16(udp->dst_port);
            return port;
        } else {
            // kprintf("[UDP packet] Not prepared to handle non-UDP packets %x",ip->protocol);
        }
    }
    else {
        // kprintf("[UDP packet] Not prepared to handle non-ipv4 packets %x",__builtin_bswap16(eth->ethertype));
    }

    return 0;
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
        } else {
            // kprintf("[UDP packet] Not prepared to handle non-UDP packets %x",ip->protocol);
        }
    }
    else {
        // kprintf("[UDP packet] Not prepared to handle non-ipv4 packets %x",__builtin_bswap16(eth->ethertype));
    }

    return (sizedptr){0,0};
}

size_t calc_udp_size(uint16_t payload_len){
    return sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + payload_len;
}