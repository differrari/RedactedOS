#pragma once
#include "networking/transport_layer/csocket.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mdns_register_service(const char *instance, const char *service, const char *proto, uint16_t port, const char *txt);
bool mdns_deregister_service(const char *instance, const char *service, const char *proto);

typedef struct {
    socket_handle_t sock;
    ip_version_t ver;
    l3_id_t l3_id;
    uint32_t l3_generation;
    uint8_t mcast_ip[16];
} mdns_tx_target_t;

void mdns_responder_tick_multi(const mdns_tx_target_t *targets, uint32_t target_count);
void mdns_responder_handle_query(socket_handle_t sock, ip_version_t ver, const uint8_t *mcast_ip, const uint8_t *pkt, uint32_t pkt_len, const net_l4_endpoint *src);

#ifdef __cplusplus
}
#endif
