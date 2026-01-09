#pragma once

#include "types.h"
#include "networking/transport_layer/csocket_udp.h"

#ifdef __cplusplus
extern "C" {
#endif

uint16_t ntp_get_pid(void);
bool ntp_is_running(void);
void ntp_set_pid(uint16_t p);
socket_handle_t ntp_socket_handle(void);

int ntp_daemon_entry(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif
