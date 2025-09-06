#pragma once

#include "types.h"
#include "net/network_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_L2_INTERFACES 16
#define MAX_IPV4_PER_INTERFACE 4
#define MAX_IPV6_PER_INTERFACE 4
#define MAX_IPV4_MCAST_PER_INTERFACE 8
#define MAX_IPV6_MCAST_PER_INTERFACE 8

typedef enum {
    NET_MODE_DISABLED = -1,
    NET_MODE_DHCP = 0,
    NET_MODE_STATIC = 1
} net_mode_t;

typedef enum {
    IPV6_ADDRK_GLOBAL = 0x01,
    IPV6_ADDRK_LINK_LOCAL = 0x02
} ipv6_addr_kind_t;

typedef enum {
    IPV6_CFG_STATIC = 0x01,
    IPV6_CFG_SLAAC = 0x02,
    IPV6_CFG_DHCPV6 = 0x04
} ipv6_cfg_t;

struct l2_interface;
struct l3_ipv4_interface;
struct l3_ipv6_interface;

typedef struct l2_interface {
    uint8_t ifindex;
    char name[16];
    uint8_t mac_addr[6];
    uint16_t mtu;
    bool is_up;
    void *tx_queue;
    void *rx_queue;
    void *driver_context;
    int (*send_frame)(struct l2_interface *iface, const void *frame, size_t len);
    void *arp_table;
    void *nd_table;
    struct l3_ipv4_interface *l3_v4[MAX_IPV4_PER_INTERFACE];
    struct l3_ipv6_interface *l3_v6[MAX_IPV6_PER_INTERFACE];
    uint8_t ipv4_count;
    uint8_t ipv6_count;
    uint32_t ipv4_mcast[MAX_IPV4_MCAST_PER_INTERFACE];
    uint8_t ipv4_mcast_count;
    uint8_t ipv6_mcast[MAX_IPV6_MCAST_PER_INTERFACE][16];
    uint8_t ipv6_mcast_count;
} l2_interface_t;

typedef struct l3_ipv4_interface {
    uint8_t l3_id;
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    uint32_t broadcast;
    net_mode_t mode;
    bool is_localhost;
    void *rt_v4;
    void *port_manager;
    l2_interface_t *l2;
} l3_ipv4_interface_t;

typedef struct l3_ipv6_interface {
    uint8_t l3_id;
    uint8_t ip[16];
    uint8_t prefix_len;
    uint8_t gateway[16];
    uint8_t kind;
    uint8_t cfg;
    bool is_localhost;
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t timestamp_created;
    uint8_t prefix[16];
    uint8_t interface_id[8];
    void *port_manager;
    l2_interface_t *l2;
} l3_ipv6_interface_t;

typedef struct ip_resolution_result {
    bool found;
    l3_ipv4_interface_t *ipv4;
    l3_ipv6_interface_t *ipv6;
    l2_interface_t *l2;
} ip_resolution_result_t;

uint8_t l2_interface_create(const char *name, void *driver_ctx);
bool l2_interface_destroy(uint8_t ifindex);
l2_interface_t *l2_interface_find_by_index(uint8_t ifindex);
uint8_t l2_interface_count(void);
l2_interface_t *l2_interface_at(uint8_t idx);
bool l2_interface_set_mac(uint8_t ifindex, const uint8_t mac[6]);
bool l2_interface_set_mtu(uint8_t ifindex, uint16_t mtu);
bool l2_interface_set_up(uint8_t ifindex, bool up);
bool l2_interface_set_send_trampoline(uint8_t ifindex, int (*fn)(struct l2_interface*, const void*, size_t));
bool l2_ipv4_mcast_join(uint8_t ifindex, uint32_t group);
bool l2_ipv4_mcast_leave(uint8_t ifindex, uint32_t group);
bool l2_ipv6_mcast_join(uint8_t ifindex, const uint8_t group[16]);
bool l2_ipv6_mcast_leave(uint8_t ifindex, const uint8_t group[16]);

uint8_t l3_ipv4_add_to_interface(uint8_t ifindex, uint32_t ip, uint32_t mask, uint32_t gw, net_mode_t mode);
bool l3_ipv4_remove_from_interface(uint8_t l3_id);
l3_ipv4_interface_t *l3_ipv4_find_by_id(uint8_t l3_id);
l3_ipv4_interface_t *l3_ipv4_find_by_ip(uint32_t ip);

uint8_t l3_ipv6_add_to_interface(uint8_t ifindex, const uint8_t ip[16], uint8_t prefix_len, const uint8_t gw[16], uint8_t cfg, uint8_t kind);
bool l3_ipv6_remove_from_interface(uint8_t l3_id);
l3_ipv6_interface_t *l3_ipv6_find_by_id(uint8_t l3_id);
l3_ipv6_interface_t *l3_ipv6_find_by_ip(const uint8_t ip[16]);

void l3_init_localhost_ipv4(void);
void l3_init_localhost_ipv6(void);
//void l3_init_both_localhost(void);

ip_resolution_result_t resolve_ipv4_to_interface(uint32_t dst_ip);
ip_resolution_result_t resolve_ipv6_to_interface(const uint8_t dst_ip[16]);

bool check_ipv4_overlap(uint32_t new_ip, uint32_t mask, uint8_t ifindex);
bool check_ipv6_overlap(const uint8_t new_ip[16], uint8_t prefix_len, uint8_t ifindex);

#ifdef __cplusplus
}
#endif