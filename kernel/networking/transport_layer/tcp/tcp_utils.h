#pragma once

#include "net/network_types.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SACK_MAX_BLOCKS 4
typedef struct {
    uint32_t left;
    uint32_t right;
} tcp_sack_block_t;

struct tcp_flow;

typedef struct {
    uint16_t mss;
    uint8_t wscale;
    uint8_t sack_permitted;
    uint8_t has_mss;
    uint8_t has_wscale;
    uint8_t sack_count;
    tcp_sack_block_t sacks[TCP_SACK_MAX_BLOCKS];
} tcp_parsed_opts_t;

void tcp_parse_options(const uint8_t *opts, uint32_t len, tcp_parsed_opts_t *out);
uint8_t tcp_build_syn_options(uint8_t *out, uint16_t mss, uint8_t wscale, uint8_t sack_permitted);

void tcp_update_mss(struct tcp_flow *flow);
uint32_t tcp_calc_mss_for_l3(uint8_t l3_id, ip_version_t ver, const void *remote_ip);
uint32_t tcp_initial_cwnd(uint32_t mss);

#ifdef __cplusplus
}
#endif