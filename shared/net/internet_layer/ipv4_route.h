#pragma once
#include "types.h"

#define IPV4_RT_MAX 8

typedef struct {
    uint32_t network;
    uint32_t mask;
    uint32_t gateway;
} ipv4_rt_entry_t;

void ipv4_rt_init();
bool ipv4_rt_add(uint32_t network, uint32_t mask, uint32_t gateway);
bool ipv4_rt_del(uint32_t network, uint32_t mask);
bool ipv4_rt_lookup(uint32_t dst, uint32_t *next_hop);
