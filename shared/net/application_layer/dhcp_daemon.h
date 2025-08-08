#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void dhcp_daemon_entry();
uint16_t get_dhcp_pid();
bool dhcp_is_running();
void dhcp_set_pid(uint16_t pid);

void dhcp_notify_link_up();
void dhcp_notify_link_down();
void dhcp_force_renew();

#ifdef __cplusplus
}
#endif
