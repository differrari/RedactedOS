#pragma once

#include "types.h"
#include "net/network_types.h"
#include "net/link_layer/eth.h"
#include "net/internet_layer/ipv4.h"
#include "net/transport_layer/udp.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DHCPDISCOVER = 1,
    DHCPOFFER = 2,
    DHCPREQUEST = 3,
    DHCPDECLINE = 4,
    DHCPACK = 5,
    DHCPNAK = 6,
    DHCPRELEASE = 7,
    DHCPINFORM = 8
};

#define DHCP_FRAME_MAX ( sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t) + sizeof(udp_hdr_t) + sizeof(dhcp_packet) )

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} dhcp_packet;

typedef struct {
    uint8_t  mac[6];
    uint32_t server_ip;
    uint32_t offered_ip;
} dhcp_request;

sizedptr dhcp_build_packet(const dhcp_request *req,
                                uint8_t msg_type,
                                uint32_t xid);

dhcp_packet* dhcp_parse_frame_payload(uintptr_t frame_ptr);

uint16_t dhcp_parse_option(const dhcp_packet *p, uint16_t wanted);

uint8_t dhcp_option_len(const dhcp_packet *p, uint16_t idx);

#ifdef __cplusplus
}
#endif
