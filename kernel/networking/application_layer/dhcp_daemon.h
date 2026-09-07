#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif


int dhcp_daemon_entry(int argc, char* argv[]);
void dhcp_daemon_kick(void);
void dhcp_force_renew();

#ifdef __cplusplus
}
#endif
