#pragma once
#include "types.h"
#include "net/network_types.h" 
#include "networking/port_manager.h"

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


void udp_send_segment(const net_l4_endpoint *src,
                    const net_l4_endpoint *dst,
                    sizedptr payload);

void udp_input(uintptr_t ptr,
            uint32_t  len,
            uint32_t  src_ip,
            uint32_t  dst_ip);

bool udp_bind(uint16_t port,
            uint16_t pid,
            port_recv_handler_t handler);

int  udp_alloc_ephemeral(uint16_t pid, port_recv_handler_t handler);

bool udp_unbind(uint16_t port,
                uint16_t pid);

#ifdef __cplusplus
}
#endif
