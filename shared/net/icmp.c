#include "icmp.h"
#include "net/udp.h"
#include "net/eth.h"
#include "net/ipv4.h"
#include "std/memfunctions.h"

void create_icmp_packet(uintptr_t p, network_connection_ctx source, network_connection_ctx destination, icmp_data* data){
    p = create_eth_packet(p, source.mac, destination.mac, 0x800);

    p = create_ipv4_packet(p, sizeof(icmp_packet), 0x01, source.ip, destination.ip);

    icmp_packet *packet = (icmp_packet*)p;

    packet->type = __builtin_bswap16(data->response ? 0 : 8);
    packet->seq = __builtin_bswap16(data->seq);
    packet->id = __builtin_bswap16(data->id);
    memcpy(packet->payload, data->payload, 56);
    packet->checksum = checksum16((uint16_t*)packet, sizeof(icmp_packet));
}

uint16_t icmp_get_sequence(icmp_packet *packet){
    return __builtin_bswap16(packet->seq);
}

uint16_t icmp_get_id(icmp_packet *packet){
    return __builtin_bswap16(packet->id);
}

void icmp_copy_payload(void* dest, icmp_packet *packet){
    memcpy(dest, packet->payload, 56);
}