#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "types.h"
#include "net/network_types.h"
#include "net/udp.h"

typedef struct __attribute__((packed)) dhcp_packet {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint8_t options[312];
} dhcp_packet;

typedef struct dhcp_request {
    uint8_t mac[6];
    uint32_t server_ip;
    uint32_t offered_ip;
} dhcp_request;

#define DHCP_SIZE sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_packet)

void create_dhcp_packet(uint8_t* buf, dhcp_request *data);
dhcp_packet* dhcp_parse_packet_payload(uintptr_t ptr);
uint16_t dhcp_parse_option(dhcp_packet *pack, uint16_t option);

#ifdef __cplusplus
}
#endif