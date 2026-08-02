#pragma once
#include "types.h"
#include "net/network_types.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/netpkt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} udp_hdr_t;

size_t create_udp_segment(uintptr_t buf,
                          const net_l4_endpoint *src,
                          const net_l4_endpoint *dst,
                          sizedptr payload);

bool udp_send_segment(const net_l4_endpoint *src, const net_l4_endpoint *dst, sizedptr payload, const ip_tx_opts_t* tx_opts, uint8_t ttl, uint8_t dontfrag);

void udp_input(ip_version_t ipver,
               const void *src_ip_addr,
               const void *dst_ip_addr,
               uint8_t l3_id,
               netpkt_t* pkt);


#ifdef __cplusplus
}
#endif
