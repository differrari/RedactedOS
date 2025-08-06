#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PORTS 65536
#define PORT_MIN_EPHEMERAL 49152
#define PORT_MAX_EPHEMERAL 65535
#define PORT_FREE_OWNER 0xFFFF

typedef enum {
    PROTO_UDP = 0,
    PROTO_TCP = 1
} protocol_t;

#define PROTO_COUNT 2

typedef void (*port_recv_handler_t)(
    uintptr_t frame_ptr,
    uint32_t frame_len,
    uint32_t src_ip,
    uint16_t src_port,
    uint16_t dst_port);

typedef struct {
    uint16_t pid;
    port_recv_handler_t handler;
    bool used;
} port_entry_t;

void port_manager_init();

int  port_alloc_ephemeral(protocol_t proto,
                        uint16_t pid,
                        port_recv_handler_t handler);

bool port_bind_manual(protocol_t proto,
                    uint16_t port,
                    uint16_t pid,
                    port_recv_handler_t handler);

bool port_unbind(protocol_t proto,
                uint16_t port,
                uint16_t pid);

void port_unbind_all(uint16_t pid);

bool port_is_bound(protocol_t proto, uint16_t port);

uint16_t port_owner_of(protocol_t proto, uint16_t port);

port_recv_handler_t port_get_handler(protocol_t proto, uint16_t port);

#ifdef __cplusplus
}
#endif
