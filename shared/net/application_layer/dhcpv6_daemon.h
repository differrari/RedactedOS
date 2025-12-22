#pragma once

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

int dhcpv6_daemon_entry(int argc, char* argv[]);

uint16_t dhcpv6_get_pid();
bool dhcpv6_is_running();
void dhcpv6_set_pid(uint16_t pid);

void dhcpv6_force_renew_all();
void dhcpv6_force_rebind_all();
void dhcpv6_force_confirm_all();

void dhcpv6_force_release_l3(uint8_t l3_id);
void dhcpv6_force_decline_l3(uint8_t l3_id);

#ifdef __cplusplus
}
#endif