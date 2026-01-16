#pragma once

#include "types.h"
#include "networking/port_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_L2_INTERFACES 15
#define MAX_IPV4_PER_INTERFACE 4
#define MAX_IPV6_PER_INTERFACE 4
#define MAX_IPV4_MCAST_PER_INTERFACE 12
#define MAX_IPV6_MCAST_PER_INTERFACE 12

typedef enum {
    IPV4_CFG_DISABLED = -1,
    IPV4_CFG_DHCP = 0,
    IPV4_CFG_STATIC = 1
} ipv4_cfg_t;

typedef enum {
    IPV6_DAD_NONE = 0,
    IPV6_DAD_IN_PROGRESS = 1,
    IPV6_DAD_FAILED = 2,
    IPV6_DAD_OK = 3
} ipv6_dad_state_t;

typedef enum {
    IPV6_ADDRK_GLOBAL = 0x01,
    IPV6_ADDRK_LINK_LOCAL = 0x02
} ipv6_addr_kind_t;

typedef enum {
    IPV6_CFG_DISABLE = -1,
    IPV6_CFG_STATIC = 0x01,
    IPV6_CFG_SLAAC = 0x02,
    IPV6_CFG_DHCPV6 = 0x04 
} ipv6_cfg_t;

struct l2_interface;
struct l3_ipv4_interface;
struct l3_ipv6_interface;

typedef struct net_runtime_opts {
    uint16_t mtu;
    uint32_t t1;
    uint32_t t2;
    uint32_t dns[2];
    uint32_t ntp[2];
    uint16_t xid;
    uint32_t server_ip;
    uint32_t lease;
    uint32_t lease_start_time;
} net_runtime_opts_t;

typedef struct l2_interface {
    uint8_t ifindex;
    char name[16];
    bool is_up;
    uint16_t base_metric;
    uint8_t kind;
    void *driver_context;
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
    ipv4_cfg_t mode;
    bool is_localhost;
    net_runtime_opts_t runtime_opts_v4;
    void *routing_table;
    port_manager_t *port_manager;
    l2_interface_t *l2;
} l3_ipv4_interface_t;

typedef struct net_runtime_opts_v6 {
    uint32_t t1;
    uint32_t t2;

    uint8_t dns[2][16];
    uint8_t ntp[2][16];

    uint32_t iaid;

    uint32_t lease;
    uint32_t lease_start_time;

    uint16_t server_id_len;
    uint8_t server_id[128];

    uint8_t pd_prefix[16];
    uint8_t pd_prefix_len;
    uint32_t pd_preferred_lft;
    uint32_t pd_valid_lft;
} net_runtime_opts_v6_t;

typedef struct l3_ipv6_interface {
    uint8_t l3_id;
    uint16_t mtu;

    uint8_t ip[16];
    uint8_t prefix_len;
    uint8_t gateway[16];
    uint8_t kind;
    ipv6_cfg_t cfg;
    bool is_localhost;
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t timestamp_created;
    uint8_t prefix[16];
    uint8_t interface_id[8];
    uint8_t dad_requested;
    ipv6_dad_state_t dad_state;
    uint8_t dad_probes_sent;
    uint32_t dad_timer_ms;
    void *routing_table;
    port_manager_t *port_manager;
    l2_interface_t *l2;
    uint8_t ra_has;
    uint8_t ra_autonomous;
    uint8_t ra_is_default;
    uint8_t ra_flags;
    uint8_t dhcpv6_stateless;
    uint8_t dhcpv6_stateless_done;
    uint32_t ra_last_update_ms;

    net_runtime_opts_v6_t runtime_opts_v6;
    uint8_t dhcpv6_state;
} l3_ipv6_interface_t;

typedef struct ip_resolution_result {
    bool found;
    l3_ipv4_interface_t *ipv4;
    l3_ipv6_interface_t *ipv6;
    l2_interface_t *l2;
} ip_resolution_result_t;

uint8_t l2_interface_create(const char *name, void *driver_ctx, uint16_t base_metric, uint8_t kind);
bool l2_interface_destroy(uint8_t ifindex);
l2_interface_t *l2_interface_find_by_index(uint8_t ifindex);
uint8_t l2_interface_count(void);
l2_interface_t *l2_interface_at(uint8_t idx);
bool l2_interface_set_up(uint8_t ifindex, bool up);

bool l2_ipv4_mcast_join(uint8_t ifindex, uint32_t group);
bool l2_ipv4_mcast_leave(uint8_t ifindex, uint32_t group);
bool l2_ipv6_mcast_join(uint8_t ifindex, const uint8_t group[16]);
bool l2_ipv6_mcast_leave(uint8_t ifindex, const uint8_t group[16]);

uint8_t l3_ipv4_add_to_interface(uint8_t ifindex, uint32_t ip, uint32_t mask, uint32_t gw, ipv4_cfg_t mode, net_runtime_opts_t *rt);
bool l3_ipv4_update(uint8_t l3_id, uint32_t ip, uint32_t mask, uint32_t gw, ipv4_cfg_t mode, net_runtime_opts_t *rt);
bool l3_ipv4_remove_from_interface(uint8_t l3_id);
l3_ipv4_interface_t *l3_ipv4_find_by_id(uint8_t l3_id);
l3_ipv4_interface_t *l3_ipv4_find_by_ip(uint32_t ip);

uint8_t l3_ipv6_add_to_interface(uint8_t ifindex, const uint8_t ip[16], uint8_t prefix_len, const uint8_t gw[16], ipv6_cfg_t cfg, uint8_t kind);
bool l3_ipv6_update(uint8_t l3_id, const uint8_t ip[16], uint8_t prefix_len, const uint8_t gw[16], ipv6_cfg_t cfg, uint8_t kind);
bool l3_ipv6_remove_from_interface(uint8_t l3_id);
bool l3_ipv6_set_enabled(uint8_t l3_id, bool enable);
l3_ipv6_interface_t *l3_ipv6_find_by_id(uint8_t l3_id);
l3_ipv6_interface_t *l3_ipv6_find_by_ip(const uint8_t ip[16]);

void l3_init_localhost_ipv4(void);
void l3_init_localhost_ipv6(void);

void ifmgr_autoconfig_all_l2(void);
void ifmgr_autoconfig_l2(uint8_t ifindex);

ip_resolution_result_t resolve_ipv4_to_interface(uint32_t dst_ip);
ip_resolution_result_t resolve_ipv6_to_interface(const uint8_t dst_ip[16]);

static inline port_manager_t* ifmgr_pm_v4(uint8_t l3_id){
    l3_ipv4_interface_t* n = l3_ipv4_find_by_id(l3_id);
    return n ? n->port_manager : NULL;
}
static inline port_manager_t* ifmgr_pm_v6(uint8_t l3_id){
    l3_ipv6_interface_t* n = l3_ipv6_find_by_id(l3_id);
    return n ? n->port_manager : NULL;
}

static inline uint8_t make_l3_id_v4(uint8_t ifindex, uint8_t local_slot){ return (uint8_t)((ifindex<<4) | (local_slot & 0x03)); }
static inline uint8_t make_l3_id_v6(uint8_t ifindex, uint8_t local_slot){ return (uint8_t)((ifindex<<4) | 0x08 | (local_slot & 0x03)); }
static inline uint8_t l3_ifindex_from_id(uint8_t l3_id){ return (uint8_t)((l3_id >> 4) & 0x0F); }
static inline uint8_t l3_is_v6_from_id(uint8_t l3_id){ return (uint8_t)((l3_id & 0x08) ? 1 : 0); }
static inline uint8_t l3_slot_from_id(uint8_t l3_id){ return (uint8_t)(l3_id & 0x03); }

#ifdef __cplusplus
}
#endif