#pragma once
#include "networking/transport_layer/csocket_udp.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mdns_register_service(const char *instance, const char *service, const char *proto, uint16_t port, const char *txt);
bool mdns_deregister_service(const char *instance, const char *service, const char *proto);

void mdns_responder_tick(socket_handle_t sock4, socket_handle_t sock6, const uint8_t mcast_v4[4], const uint8_t mcast_v6[16]);
void mdns_responder_handle_query(socket_handle_t sock, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len, const net_l4_endpoint *src);

#ifdef __cplusplus
}
#endif
