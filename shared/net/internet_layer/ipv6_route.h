#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPV6_RT_PER_IF_MAX 32

typedef struct {
    uint8_t network[16];
    uint8_t prefix_len;
    uint8_t gateway[16];
    uint16_t metric;
} ipv6_rt_entry_t;

typedef struct ipv6_rt_table ipv6_rt_table_t;

ipv6_rt_table_t* ipv6_rt_create(void);
void ipv6_rt_destroy(ipv6_rt_table_t* t);
void ipv6_rt_clear(ipv6_rt_table_t* t);

bool ipv6_rt_add_in(ipv6_rt_table_t* t, const uint8_t net[16], uint8_t plen, const uint8_t gw[16], uint16_t metric);
bool ipv6_rt_del_in(ipv6_rt_table_t* t, const uint8_t net[16], uint8_t plen);
bool ipv6_rt_lookup_in(const ipv6_rt_table_t* t, const uint8_t dst[16], uint8_t next_hop[16], int* out_prefix_len, int* out_metric);

void ipv6_rt_ensure_basics(ipv6_rt_table_t* t, const uint8_t ip[16], uint8_t plen, const uint8_t gw[16], uint16_t base_metric);
void ipv6_rt_sync_basics(ipv6_rt_table_t* t, const uint8_t ip[16], uint8_t plen, const uint8_t gw[16], uint16_t base_metric);

bool ipv6_rt_pick_best_l3_in(const uint8_t* l3_ids, int n_ids, const uint8_t dst[16], uint8_t* out_l3);

typedef struct {
    uint8_t l3_id;
    uint8_t src_ip[16];
    ip_tx_opts_t fixed_opts;
} ipv6_tx_plan_t;

bool ipv6_build_tx_plan(const uint8_t dst[16], const ip_tx_opts_t* hint, const uint8_t* allowed_l3, int allowed_n, ipv6_tx_plan_t* out);

#ifdef __cplusplus
}
#endif