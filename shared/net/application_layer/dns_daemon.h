#pragma once
#include "net/transport_layer/csocket_udp.h"

#ifdef __cplusplus
extern "C" {
#endif
    bool dns_is_running(void);
    void dns_set_pid(uint16_t p);
    socket_handle_t dns_socket_handle(void);
    uint16_t dns_get_pid(void);
    void dns_deamon_entry(void);
#ifdef __cplusplus
}
#endif
