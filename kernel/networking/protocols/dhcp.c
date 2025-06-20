#include "dhcp.h"
#include "std/memfunctions.h"

void create_dhcp_packet(uint8_t* buf, uint8_t mac[6]){
    network_connection_ctx source = (network_connection_ctx){
        .port = 68,
    };
    network_connection_ctx destination = (network_connection_ctx){
        .ip = (255 << 24) | (255 << 16) | (255 << 8) | 255,
        .mac = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        .port = 67,
    };
    dhcp_packet packet = (dhcp_packet){
        .op = 1,//request
        .htype = 1,//Ethernet
        .hlen = 6,//Mac length
        .hops = 0,
        .xid = 372,//Transaction ID: Static, could be random
        .secs = 0,
        .flags = __builtin_bswap16(0x8000),//Broadcast
        .ciaddr = 0,
        .yiaddr = 0,
        .siaddr = 0,
        .giaddr = 0,
    };
    memcpy(packet.chaddr, mac, 6);
    memcpy(source.mac, mac, 6);

    packet.options[0] = 0x63; // magic
    packet.options[1] = 0x82;
    packet.options[2] = 0x53;
    packet.options[3] = 0x63; // magic

    packet.options[4] = 53; // DHCP type
    packet.options[5] = 1; // length
    packet.options[6] = 1; // DHCPDISCOVER

    packet.options[7] = 255; // END
    create_udp_packet(buf,  source, destination, (uint8_t*)&packet, sizeof(dhcp_packet));
}

dhcp_packet* dhcp_parse_packet_payload(uintptr_t ptr){
    sizedptr sptr = udp_parse_packet_payload(ptr);
    return (dhcp_packet*)sptr.ptr;
}