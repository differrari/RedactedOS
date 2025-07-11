#include "dhcp.h"
#include "std/memfunctions.h"
#include "math/random.h"

void create_dhcp_packet(uintptr_t p, dhcp_request *payload){
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
        .xid = rng_next32(&global_rng),//Transaction ID: RANDOM
        .secs = 0,
        .flags = __builtin_bswap16(0x8000),//Broadcast
        .ciaddr = 0,
        .yiaddr = 0,
        .siaddr = 0,
        .giaddr = 0,
    };
    memcpy(packet.chaddr, payload->mac, 6);
    memcpy(source.mac, payload->mac, 6);

    packet.options[0] = 0x63; // magic
    packet.options[1] = 0x82;
    packet.options[2] = 0x53;
    packet.options[3] = 0x63; // magic

    packet.options[4] = 53; // DHCP type
    packet.options[5] = 1; // length
    if (payload->server_ip != 0 && payload->offered_ip != 0){
        packet.options[6] = 3; // DHCPREQUEST

        packet.options[7] = 50;
        packet.options[8] = 4;
        memcpy(&packet.options[9], &payload->offered_ip, 4);

        packet.options[13] = 54;
        packet.options[14] = 4;
        memcpy(&packet.options[15], &payload->server_ip, 4);
        packet.options[19] = 255;
    } else {
        packet.options[6] = 1; // DHCPDISCOVER

        packet.options[7] = 255; // END
    }
    
    create_udp_packet(p, source, destination, (sizedptr){(uintptr_t)&packet, sizeof(dhcp_packet)});
}

dhcp_packet* dhcp_parse_packet_payload(uintptr_t ptr){
    sizedptr sptr = udp_parse_packet_payload(ptr);
    return (dhcp_packet*)sptr.ptr;
}

uint16_t dhcp_parse_option(dhcp_packet *pack, uint16_t option){
    for (int i = 0; i < 312; i++)
        if (pack->options[i] == option) return i;

    return 0;
}


