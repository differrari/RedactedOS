#pragma once

#include "types.h"
#include "net/network_types.h"
#include "net/socket_types.h"
#include "networking/transport_layer/socket_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOCKET_BIND_MAX 1024
#define SOCKET_BIND_COLLECT_MAX 32

bool socket_bind_insert(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, uint16_t port);
void socket_bind_remove_socket(ksocket_t* socket);
int socket_bind_alloc_ephemeral_l3(ksocket_t* socket, protocol_t protocol, uint8_t l3_id, uint16_t pid);

uint32_t socket_bind_collect(protocol_t protocol, ip_version_t ipver, uint8_t l3_id, uint8_t ifindex, const void* dst_ip_addr, uint16_t dst_port, ksocket_t** out, uint32_t out_cap);

bool socket_bind_port_busy(protocol_t protocol, ip_version_t ipver, uint8_t l3_id, uint16_t port);
uint32_t socket_bind_l3_list(const SockBindSpec* spec, ip_version_t ver, uint8_t* out, uint32_t cap);

#ifdef __cplusplus
}
#endif
