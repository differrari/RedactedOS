#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

void dhcp_daemon_entry(void);
uint16_t get_dhcp_pid(void);
bool dhcp_is_running(void);
void dhcp_set_pid(uint16_t pid);

void dhcp_notify_link_up(void);
void dhcp_notify_link_down(void);
void dhcp_force_renew(void);

#ifdef __cplusplus
}
#endif
