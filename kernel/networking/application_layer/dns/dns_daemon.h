#pragma once
#include "networking/transport_layer/csocket.h"
#include "net/network_types.h"

#define IPV4_MCAST_MDNS 0xE00000FBu

#ifdef __cplusplus
extern "C" {
#endif
socket_handle_t mdns_socket_handle_for(ip_version_t ver);
socket_handle_t mdns_socket_handle_for_l3(l3_id_t l3_id);

int dns_deamon_entry(int argc, char* argv[]);
#ifdef __cplusplus
}
#endif
