#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void* socket_handle_t;

socket_handle_t udp_socket_create(uint8_t role, uint32_t pid);

int32_t socket_bind_udp(socket_handle_t sh, uint16_t port);

int64_t socket_sendto_udp(socket_handle_t sh,
                        uint32_t ip, uint16_t port,
                        const void* buf, uint64_t len);

int64_t socket_recvfrom_udp(socket_handle_t sh,
                        void* buf, uint64_t len,
                        uint32_t* out_ip, uint16_t* out_port);

int32_t socket_close_udp(socket_handle_t sh);

void socket_destroy_udp(socket_handle_t sh);

uint16_t socket_get_local_port_udp(socket_handle_t sh);

uint16_t socket_get_remote_port_udp(socket_handle_t sh);

uint32_t socket_get_remote_ip_udp(socket_handle_t sh);

uint8_t socket_get_protocol_udp(socket_handle_t sh);

uint8_t socket_get_role_udp(socket_handle_t sh);

bool socket_is_bound_udp(socket_handle_t sh);

bool socket_is_connected_udp(socket_handle_t sh);
#ifdef __cplusplus
}
#endif
