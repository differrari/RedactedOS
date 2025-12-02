#pragma once
#include "networking/transport_layer/csocket_udp.h"

#ifdef __cplusplus
extern "C" {
#endif
    bool dns_is_running(void);
    void dns_set_pid(uint16_t p);
    socket_handle_t dns_socket_handle(void);
    uint16_t dns_get_pid(void);
    int dns_deamon_entry(int argc, char* argv[]);
#ifdef __cplusplus
}
#endif
