#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PORTS 65536
#define PORT_MIN_EPHEMERAL 49152
#define PORT_MAX_EPHEMERAL 65535
#define PORT_FREE_OWNER 0xFFFF

#define PROTO_COUNT 2

typedef uint32_t (*port_recv_handler_t)(
    uint8_t ifindex,
    ip_version_t ipver,
    const void* src_ip_addr,
    const void* dst_ip_addr,
    uintptr_t frame_ptr,
    uint32_t frame_len,
    uint16_t src_port,
    uint16_t dst_port
);

typedef struct {
    uint16_t pid;
    port_recv_handler_t handler;
    bool used;
} port_entry_t;

typedef struct {
    port_entry_t tab[PROTO_COUNT][MAX_PORTS];
} port_manager_t;

void port_manager_init(port_manager_t* pm);

int  port_alloc_ephemeral(port_manager_t* pm,
                          protocol_t proto,
                          uint16_t pid,
                          port_recv_handler_t handler);

bool port_bind_manual(port_manager_t* pm,
                      protocol_t proto,
                      uint16_t port,
                      uint16_t pid,
                      port_recv_handler_t handler);

bool port_unbind(port_manager_t* pm,
                 protocol_t proto,
                 uint16_t port,
                 uint16_t pid);

void port_unbind_all(port_manager_t* pm, uint16_t pid);

bool port_is_bound(const port_manager_t* pm, protocol_t proto, uint16_t port);
uint16_t port_owner_of(const port_manager_t* pm, protocol_t proto, uint16_t port);
port_recv_handler_t port_get_handler(const port_manager_t* pm, protocol_t proto, uint16_t port);

#ifdef __cplusplus
}
#endif
