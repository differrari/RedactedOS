#pragma once
#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPV4_RT_PER_IF_MAX 32

typedef struct {
    uint32_t network;
    uint32_t mask;
    uint32_t gateway;
    uint16_t metric;
} ipv4_rt_entry_t;

typedef struct ipv4_rt_table ipv4_rt_table_t;

ipv4_rt_table_t* ipv4_rt_create(void);
void ipv4_rt_destroy(ipv4_rt_table_t* t);
void ipv4_rt_clear(ipv4_rt_table_t* t);

bool ipv4_rt_add_in(ipv4_rt_table_t* t, uint32_t network, uint32_t mask, uint32_t gateway, uint16_t metric);
bool ipv4_rt_del_in(ipv4_rt_table_t* t, uint32_t network, uint32_t mask);

bool ipv4_rt_lookup_in(const ipv4_rt_table_t* t, uint32_t dst, uint32_t *next_hop, int* out_prefix_len, int* out_metric);

void ipv4_rt_ensure_basics(ipv4_rt_table_t* t, uint32_t ip, uint32_t mask, uint32_t gw, uint16_t base_metric);
void ipv4_rt_sync_basics(ipv4_rt_table_t* t, uint32_t ip, uint32_t mask, uint32_t gw, uint16_t base_metric);

typedef struct {
    uint8_t l3_id;
    uint32_t src_ip;
    ip_tx_opts_t fixed_opts;
} ipv4_tx_plan_t;

bool ipv4_build_tx_plan(uint32_t dst, const ip_tx_opts_t* hint, const uint8_t* allowed_l3, int allowed_n, ipv4_tx_plan_t* out);

bool ipv4_rt_pick_best_l3_in(const uint8_t* l3_ids, int n_ids, uint32_t dst, uint8_t* out_l3);

#ifdef __cplusplus
}
#endif