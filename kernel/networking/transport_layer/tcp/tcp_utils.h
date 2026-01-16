#pragma once

#include "net/network_types.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/port_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t mss;
    uint8_t wscale;
    uint8_t sack_permitted;
    uint8_t has_mss;
    uint8_t has_wscale;
} tcp_parsed_opts_t;

void tcp_parse_options(const uint8_t *opts, uint32_t len, tcp_parsed_opts_t *out);
uint8_t tcp_build_syn_options(uint8_t *out, uint16_t mss, uint8_t wscale, uint8_t sack_permitted);

port_manager_t *tcp_pm_for_l3(uint8_t l3_id);
uint32_t tcp_calc_mss_for_l3(uint8_t l3_id, ip_version_t ver, const void *remote_ip);

bool tcp_build_tx_opts_from_local_v4(const void *src_ip_addr, ipv4_tx_opts_t *out);
bool tcp_build_tx_opts_from_l3(uint8_t l3_id, ipv4_tx_opts_t *out);
bool tcp_build_tx_opts_from_local_v6(const void *src_ip_addr, ipv6_tx_opts_t *out);

#ifdef __cplusplus
}
#endif