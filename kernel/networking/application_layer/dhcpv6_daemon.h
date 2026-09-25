#pragma once

#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

int dhcpv6_daemon_entry(int argc, char* argv[]);

void dhcpv6_daemon_kick(void);

void dhcpv6_force_renew_all();
void dhcpv6_force_rebind_all();
void dhcpv6_force_confirm_all();

void dhcpv6_force_release_l3(l3_id_t l3_id);
void dhcpv6_force_decline_l3(l3_id_t l3_id);

#ifdef __cplusplus
}
#endif