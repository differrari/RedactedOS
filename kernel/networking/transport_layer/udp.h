#pragma once
#include "types.h"
#include "net/network_types.h"
#include "networking/port_manager.h"
#include "networking/internet_layer/ipv4.h"

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

void udp_send_segment(const net_l4_endpoint *src, const net_l4_endpoint *dst, sizedptr payload, const ip_tx_opts_t* tx_opts, uint8_t ttl, uint8_t dontfrag);

void udp_input(ip_version_t ipver,
               const void *src_ip_addr,
               const void *dst_ip_addr,
               uint8_t l3_id,
               uintptr_t ptr,
               uint32_t len);

bool udp_bind_l3(uint8_t l3_id, uint16_t port, uint16_t pid, port_recv_handler_t handler);
bool udp_unbind_l3(uint8_t l3_id, uint16_t port, uint16_t pid);
int  udp_alloc_ephemeral_l3(uint8_t l3_id, uint16_t pid, port_recv_handler_t handler);

#ifdef __cplusplus
}
#endif
