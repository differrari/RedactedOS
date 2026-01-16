#pragma once
#include "net_driver.hpp"

#include "networking/link_layer/nic_types.h"

int net_bus_init();
int net_bus_count();
NetDriver* net_bus_driver(int idx);
const char* net_bus_ifname(int idx);
const char* net_bus_hw_ifname(int idx);
void net_bus_get_mac(int idx, uint8_t out_mac[6]);
uint16_t net_bus_get_mtu(int idx);
uint16_t net_bus_get_header_size(int idx);
uint8_t net_bus_get_kind(int idx);
uint32_t net_bus_get_speed_mbps(int idx);
uint8_t net_bus_get_duplex(int idx);