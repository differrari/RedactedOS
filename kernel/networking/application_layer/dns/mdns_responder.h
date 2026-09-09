#pragma once
#include "networking/transport_layer/csocket.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mdns_register_service(const char *instance, const char *service, const char *proto, uint16_t port, const char *txt);
bool mdns_deregister_service(const char *instance, const char *service, const char *proto);

#ifdef __cplusplus
}
#endif
