#pragma once

#include "types.h"
#include "net/net_ctrl.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool firewall_allows(protocol_t protocol, NetCtrlFirewallDirection direction, const net_l4_endpoint* remote, uint16_t local_port, bool related);
uint32_t firewall_get_snapshot(NetCtrlFirewallState* state, NetCtrlFirewallRule* out, uint32_t max_rules);
int32_t firewall_add_rule(const NetCtrlFirewallRule* rule, uint16_t owner_pid);
int32_t firewall_remove_rule(uint32_t id);
void firewall_cleanup_process(uint16_t pid);
void firewall_clear_rules(void);
void firewall_set_enabled(bool value);
int32_t firewall_set_default(NetCtrlFirewallDirection direction, NetCtrlFirewallAction action);

#ifdef __cplusplus
}
#endif
