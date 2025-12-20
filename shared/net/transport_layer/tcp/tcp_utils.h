#pragma once

#include "net/network_types.h"
#include "net/internet_layer/ipv4.h"
#include "net/internet_layer/ipv6.h"
#include "net/internet_layer/ipv4_utils.h"
#include "net/internet_layer/ipv6_utils.h"
#include "networking/port_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

port_manager_t *tcp_pm_for_l3(uint8_t l3_id);

bool tcp_build_tx_opts_from_local_v4(const void *src_ip_addr, ipv4_tx_opts_t *out);
bool tcp_build_tx_opts_from_l3(uint8_t l3_id, ipv4_tx_opts_t *out);
bool tcp_build_tx_opts_from_local_v6(const void *src_ip_addr, ipv6_tx_opts_t *out);

#ifdef __cplusplus
}
#endif