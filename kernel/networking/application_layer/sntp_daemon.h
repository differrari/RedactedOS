#pragma once//deprecated, use ntp
#include "types.h"
#include "networking/transport_layer/csocket_udp.h"

#ifdef __cplusplus
extern "C" {
#endif

uint16_t sntp_get_pid(void);
bool sntp_is_running(void);
void sntp_set_pid(uint16_t p);
socket_handle_t sntp_socket_handle(void);
int sntp_daemon_entry(int argc, char* argv[]);

#ifdef __cplusplus
}
#endif
