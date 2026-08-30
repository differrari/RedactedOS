#include "interface_manager.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/link_layer/arp.h"
#include "networking/link_layer/link_utils.h"
#include "networking/link_layer/ndp.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv6_route.h"
#include "process/scheduler.h"
#include "memory/page_allocator.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/igmp.h"
#include "networking/internet_layer/mld.h"
#include "networking/link_layer/nic_types.h"
#include "networking/network.h"
//TODO: add network settings

static l2_interface_t g_l2[MAX_L2_INTERFACES];
static uint8_t g_l2_used[MAX_L2_INTERFACES];
static uint8_t g_l2_count = 0;
static uint32_t g_if_epoch = 1;

typedef struct {
    l3_ipv4_interface_t node;
    bool used;
} v4_slot_t;
typedef struct {
    l3_ipv6_interface_t node;
    bool used;
} v6_slot_t;

static v4_slot_t g_v4[MAX_IPV4_L3_INTERFACES];
static v6_slot_t g_v6[MAX_IPV6_L3_INTERFACES];

static uint32_t net_interface_mark_changed(void) {
    g_if_epoch++;
    if (!g_if_epoch) g_if_epoch = 1;
    return g_if_epoch;
}

static inline int l2_slot_from_ifindex(uint8_t ifindex){
    if (!ifindex) return -1;
    int s = (int)ifindex - 1;
    if (s<0 || s>=(int)MAX_L2_INTERFACES) return -1;
    if (!g_l2_used[s]) return -1;
    return s;
}

static bool v4_has_dhcp_on_l2(uint8_t ifindex){
    for (int i = 0; i < MAX_IPV4_L3_INTERFACES; i++){
        if (!g_v4[i].used) continue;
        l3_ipv4_interface_t *x = &g_v4[i].node;
        if (!x->l2) continue;
        if (x->l2->ifindex != ifindex) continue;
        if (x->mode == IPV4_CFG_DHCP) return true;
    }
    return false;
}

static bool l2_has_active_v4(l2_interface_t* itf) {
    if (!itf) return false;
    for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
        l3_ipv4_interface_t* v4 = itf->l3_v4[s];
        if (!v4) continue;
        if (v4->mode == IPV4_CFG_DISABLED) continue;
        if (v4->ip) return true;
    }
    return false;
}

static bool l2_has_active_v6(l2_interface_t* itf) {
    if (!itf) return false;
    for (int s = 0; s < MAX_IPV6_PER_INTERFACE; s++) {
        l3_ipv6_interface_t* v6 = itf->l3_v6[s];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (!ipv6_is_unspecified(v6->ip)) return true;
    }
    return false;
}

static bool l2_sync_multicast_filters(l2_interface_t* itf) {
    if (!itf) return false;
    uint8_t macs[(MAX_IPV4_MCAST_PER_INTERFACE + MAX_IPV6_MCAST_PER_INTERFACE) * MAC_ADDR_LEN];
    uint32_t count = 0;

    for (int i = 0; i < (int)itf->ipv4_mcast_count; i++) {
        uint8_t m[MAC_ADDR_LEN];
        ipv4_mcast_to_mac(itf->ipv4_mcast[i], m);
        bool exists = false;
        for (uint32_t j = 0; j < count; j++) {
            if (mac_equal(&macs[j * MAC_ADDR_LEN], m)){
                exists = true;
                break;
            }
        }
        if (!exists) {
            mac_copy(&macs[count * MAC_ADDR_LEN], m);
            count++;
        }
    }

    for (int i = 0; i < (int)itf->ipv6_mcast_count; i++) {
        uint8_t m[MAC_ADDR_LEN];
        ipv6_multicast_mac(itf->ipv6_mcast[i], m);
        bool exists = false;
        for (uint32_t j = 0; j < count; j++) {
            if (mac_equal(&macs[j * MAC_ADDR_LEN], m)){
                exists = true;
                break;
            }
        }
        if (!exists) {
            mac_copy(&macs[count * MAC_ADDR_LEN], m);
            count++;
        }
    }

    return network_sync_multicast(itf->ifindex, macs, count);
}

uint8_t l2_interface_create(const char *name, uint8_t nic_id, uint16_t base_metric, uint8_t kind) {
    int slot = -1;
    for (int i=0;i<(int)MAX_L2_INTERFACES;i++) if (!g_l2_used[i]) {
        slot=i;
        break;
    }
    if (slot < 0) return 0;

    l2_interface_t* itf = &g_l2[slot];
    memset(itf, 0, sizeof(*itf));
    itf->ifindex = (uint8_t)(slot + 1);
    itf->nic_id = nic_id;

    if (name) strncpy(itf->name, name, sizeof(itf->name));

    itf->base_metric = base_metric;
    itf->kind = kind;
    if (kind != NET_IFK_LOCALHOST) {
        itf->arp_table = arp_table_create();
        if (!itf->arp_table) {
            memset(itf, 0, sizeof(*itf));
            return 0;
        }
        itf->nd_table = ndp_table_create();
        if (!itf->nd_table) {
            arp_table_destroy((arp_table_t*)itf->arp_table);
            memset(itf, 0, sizeof(*itf));
            return 0;
        }
    } else {
        itf->arp_table = NULL;
        itf->nd_table = NULL;
    }

    itf->ipv4_mcast[0] = IPV4_MCAST_ALL_HOSTS;
    itf->ipv4_mcast_ref[0] = 1;
    itf->ipv4_mcast_count = 1;

    uint8_t all_nodes[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
    ipv6_cpy(itf->ipv6_mcast[0], all_nodes);
    itf->ipv6_mcast_ref[0] = 1;
    itf->ipv6_mcast_count = 1;

    g_l2_used[slot] = 1;
    g_l2_count += 1;
    net_interface_mark_changed();
    return itf->ifindex;
}

bool l2_interface_destroy(uint8_t ifindex){
    int slot = l2_slot_from_ifindex(ifindex);
    if (slot < 0) return false;
    l2_interface_t* itf = &g_l2[slot];
    if (itf->ipv4_count || itf->ipv6_count) return false;

    if (itf->arp_table) {
        arp_table_destroy((arp_table_t*)itf->arp_table);
        itf->arp_table = NULL;
    }
    if (itf->nd_table) {
        ndp_table_destroy((ndp_table_t*)itf->nd_table);
        itf->nd_table = NULL;
    }

    memset(&g_l2[slot], 0, sizeof(l2_interface_t));
    g_l2_used[slot] = 0;
    if (g_l2_count) g_l2_count -= 1;
    net_interface_mark_changed();
    return true;
}

l2_interface_t* l2_interface_find_by_index(uint8_t ifindex) {
    int slot = l2_slot_from_ifindex(ifindex);
    if (slot < 0) return 0;
    return &g_l2[slot];
}

uint8_t l2_interface_count(void) { return g_l2_count; }

l2_interface_t* l2_interface_at(uint8_t idx) {
    uint8_t seen = 0;
    for (int i=0;i<(int)MAX_L2_INTERFACES;i++){
        if (!g_l2_used[i]) continue;
        if (seen == idx) return &g_l2[i];
        seen++;
    }
    return 0;
}

bool l2_interface_set_up(uint8_t ifindex, bool up) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    if (itf->is_up == up) return true;
    itf->is_up = up;
    if (up && itf->kind != NET_IFK_LOCALHOST) (void)l2_sync_multicast_filters(itf);
    net_interface_mark_changed();
    return true;
}

bool l2_interface_set_metric(uint8_t ifindex, uint16_t metric) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    if (itf->base_metric == metric) return true;
    itf->base_metric = metric;
    for (int i = 0; i < MAX_IPV4_PER_INTERFACE; i++) {
        l3_ipv4_interface_t* v4 = itf->l3_v4[i];
        if (!v4 || !v4->routing_table) continue;
        ipv4_rt_sync_basics((ipv4_rt_table_t*)v4->routing_table, v4->ip, v4->mask, v4->gw, itf->base_metric);
    }
    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = itf->l3_v6[i];
        if (!v6 || !v6->routing_table) continue;
        ipv6_rt_sync_basics((ipv6_rt_table_t*)v6->routing_table, v6->ip, v6->prefix_len, v6->gateway, itf->base_metric);
    }
    net_interface_mark_changed();
    return true;
}


bool l2_ipv4_mcast_join(uint8_t ifindex, uint32_t group) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    if (!ipv4_is_multicast(group)) return false;
    int idx = -1;
    for (int i = 0; i < (int)itf->ipv4_mcast_count; i++) {
        if (itf->ipv4_mcast[i] != group) continue;
        idx = i;
        break;
    }
    if (idx >= 0) {
        if (itf->ipv4_mcast_ref[idx] < 0xFFFFu) itf->ipv4_mcast_ref[idx] += 1;
        return true;
    }
    if (itf->ipv4_mcast_count >= MAX_IPV4_MCAST_PER_INTERFACE) return false;
    itf->ipv4_mcast[itf->ipv4_mcast_count] = group;
    itf->ipv4_mcast_ref[itf->ipv4_mcast_count] = 1;
    itf->ipv4_mcast_count += 1;
    if (itf->kind != NET_IFK_LOCALHOST) (void)l2_sync_multicast_filters(itf);
    if (itf->kind != NET_IFK_LOCALHOST && l2_has_active_v4(itf)) (void)igmp_send_join(ifindex, group);
    return true;
}

bool l2_ipv4_mcast_leave(uint8_t ifindex, uint32_t group) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    int idx = -1;
    for (int i = 0; i < (int)itf->ipv4_mcast_count; i++) {
        if (itf->ipv4_mcast[i] != group) continue;
        idx = i;
        break;
    }
    if (idx < 0) return true;
    if (group == IPV4_MCAST_ALL_HOSTS && itf->ipv4_mcast_ref[idx] <= 1) return true;
    if (itf->ipv4_mcast_ref[idx] > 1) {
        itf->ipv4_mcast_ref[idx] -= 1;
        return true;
    }
    for (int i = idx + 1; i < (int)itf->ipv4_mcast_count; i++) {
        itf->ipv4_mcast[i-1] = itf->ipv4_mcast[i];
        itf->ipv4_mcast_ref[i-1] = itf->ipv4_mcast_ref[i];
    }
    if (itf->ipv4_mcast_count) itf->ipv4_mcast_count -= 1;
    if (itf->ipv4_mcast_count < MAX_IPV4_MCAST_PER_INTERFACE) itf->ipv4_mcast_ref[itf->ipv4_mcast_count] = 0;
    if (itf->kind != NET_IFK_LOCALHOST) (void)l2_sync_multicast_filters(itf);
    if (itf->kind != NET_IFK_LOCALHOST && l2_has_active_v4(itf)) (void)igmp_send_leave(ifindex, group);
    return true;
}

bool l2_ipv6_mcast_join(uint8_t ifindex, const uint8_t group[16]) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf || !group) return false;
    if (!ipv6_is_multicast(group)) return false;
    int idx = -1;
    for (int i = 0; i < (int)itf->ipv6_mcast_count; i++) {
        if (ipv6_cmp(itf->ipv6_mcast[i], group) != 0) continue;
        idx = i;
        break;
    }
    if (idx >= 0) {
        if (itf->ipv6_mcast_ref[idx] < 0xFFFFu) itf->ipv6_mcast_ref[idx] += 1;
        return true;
    }
    if (itf->ipv6_mcast_count >= MAX_IPV6_MCAST_PER_INTERFACE) return false;
    ipv6_cpy(itf->ipv6_mcast[itf->ipv6_mcast_count], group);
    itf->ipv6_mcast_ref[itf->ipv6_mcast_count] = 1;
    itf->ipv6_mcast_count += 1;
    if (itf->kind != NET_IFK_LOCALHOST) (void)l2_sync_multicast_filters(itf);
    if (itf->kind != NET_IFK_LOCALHOST && l2_has_active_v6(itf)) (void)mld_send_join(ifindex, group);
    return true;
}
bool l2_ipv6_mcast_leave(uint8_t ifindex, const uint8_t group[16]) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf || !group) return false;
    int idx = -1;
    for (int i = 0; i < (int)itf->ipv6_mcast_count; i++) {
        if (ipv6_cmp(itf->ipv6_mcast[i], group) != 0) continue;
        idx = i;
        break;
    }
    if (idx < 0) return true;

    uint8_t all_nodes[16];
    ipv6_make_multicast(2, IPV6_MCAST_ALL_NODES, NULL, all_nodes);
    if (ipv6_cmp(group, all_nodes) == 0 && itf->ipv6_mcast_ref[idx] <= 1) return true;
    if (itf->ipv6_mcast_ref[idx] > 1) {
        itf->ipv6_mcast_ref[idx] -= 1;
        return true;
    }
    if (itf->kind != NET_IFK_LOCALHOST && l2_has_active_v6(itf)) (void)mld_send_leave(ifindex, group);
    for (int i = idx + 1; i < (int)itf->ipv6_mcast_count; i++) {
        ipv6_cpy(itf->ipv6_mcast[i-1], itf->ipv6_mcast[i]);
        itf->ipv6_mcast_ref[i-1] = itf->ipv6_mcast_ref[i];
    }
    if (itf->ipv6_mcast_count) itf->ipv6_mcast_count -= 1;
    if (itf->ipv6_mcast_count < MAX_IPV6_MCAST_PER_INTERFACE) itf->ipv6_mcast_ref[itf->ipv6_mcast_count] = 0;
    if (itf->kind != NET_IFK_LOCALHOST) (void)l2_sync_multicast_filters(itf);
    return true;
}

static bool v4_ip_exists_anywhere(uint32_t ip){
    for (int i=0;i<MAX_IPV4_L3_INTERFACES;i++) if (g_v4[i].used && g_v4[i].node.ip == ip) return true;
    return false;
}

static bool v4_overlap_intra_l2(uint8_t ifindex, uint32_t ip, uint32_t mask){
    if (!ipv4_mask_is_contiguous(mask)) return true;
    for (int i=0;i<MAX_IPV4_L3_INTERFACES;i++){
        if (!g_v4[i].used) continue;
        l3_ipv4_interface_t *x = &g_v4[i].node;
        if (!x->l2 || x->l2->ifindex != ifindex) continue;
        if (x->mode == IPV4_CFG_DISABLED) continue;
        uint32_t m = (x->mask==0)?mask:((mask==0)?x->mask:((x->mask < mask)?x->mask:mask));
        if (ipv4_net(ip, m) != ipv4_net(x->ip, m)) continue;
        if (x->mask == mask && ipv4_net(ip, mask) == ipv4_net(x->ip, x->mask)) continue;
        return true;
    }
    return false;
}

static bool v6_ip_exists_anywhere(const uint8_t ip[16]){
    if (ipv6_is_unspecified(ip)) return false;
    for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++) {
        if (g_v6[i].used && ipv6_cmp(g_v6[i].node.ip, ip)==0) return true;
    }
    return false;
}

static bool v6_overlap_intra_l2(uint8_t ifindex, const uint8_t ip[16], uint8_t prefix_len, const l3_ipv6_interface_t *except){
    if (ipv6_is_unspecified(ip)) return false;
    for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
        if (!g_v6[i].used) continue;
        l3_ipv6_interface_t *x = &g_v6[i].node;
        if (x == except) continue;
        if (!x->l2 || x->l2->ifindex != ifindex) continue;
        if (x->cfg == IPV6_CFG_DISABLE) continue;
        if (ipv6_is_unspecified(x->ip)) continue;
        uint8_t minp = (x->prefix_len < prefix_len) ? x->prefix_len : prefix_len;
        if (ipv6_common_prefix_len(ip, x->ip) >= minp) return true;
    }
    return false;
}

l3_id_t l3_ipv4_add_to_interface(uint8_t ifindex, uint32_t ip, uint32_t mask, uint32_t gw, ipv4_cfg_t mode, net_runtime_opts_t *runtime_opts) {
    l2_interface_t *l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return 0;
    bool had_active_v4 = l2_has_active_v4(l2);
    if (mode == IPV4_CFG_DHCP) {
        if (v4_has_dhcp_on_l2(ifindex)) {
            return 0;
        }
    }
    if (mode == IPV4_CFG_STATIC){
        if (ipv4_is_unspecified(ip)) return 0;
        if (!ipv4_mask_is_contiguous(mask)) return 0;
        if (ipv4_is_loopback(ip) && (l2->kind != NET_IFK_LOCALHOST)) return 0;
        if (ipv4_is_multicast(ip)) return 0;
        if (ipv4_is_reserved_special(ip)) {
            if (!(ipv4_is_loopback(ip) && l2->kind == NET_IFK_LOCALHOST)) return 0;
        }
        if (ipv4_is_network_address(ip, mask)) return 0;
        if (ipv4_is_broadcast_address(ip, mask)) return 0;
        if (v4_ip_exists_anywhere(ip)) return 0;
        if (v4_overlap_intra_l2(ifindex, ip, mask)) return 0;
        if (l2->kind != NET_IFK_LOCALHOST && !arp_dad_ipv4_on(ifindex, ip)) return 0;
    }
    if (l2->ipv4_count >= MAX_IPV4_PER_INTERFACE) return 0;

    int loc = -1;
    for (int s=0; s<MAX_IPV4_PER_INTERFACE; s++) if (l2->l3_v4[s] == NULL) {
        loc = s;
        break;
    }
    int g = -1;
    for (int i=0;i<MAX_IPV4_L3_INTERFACES;i++) if (!g_v4[i].used) {
        g = i;
        break;
    }
    if (loc < 0 || g < 0) return 0;

    memset(&g_v4[g], 0, sizeof(g_v4[g]));
    l3_ipv4_interface_t *n = &g_v4[g].node;
    n->l2 = l2;
    n->mode = mode;
    n->ip = (mode==IPV4_CFG_STATIC) ? ip : 0;
    n->mask = (mode==IPV4_CFG_STATIC) ? mask : 0;
    n->gw = (mode==IPV4_CFG_STATIC) ? gw : 0;
    n->broadcast = (mode==IPV4_CFG_STATIC) ? ipv4_broadcast_calc(ip, mask) : 0;

    memset(&n->runtime_opts_v4, 0, sizeof(n->runtime_opts_v4));
    if (runtime_opts) n->runtime_opts_v4 = *runtime_opts;

    n->is_localhost = (l2->kind == NET_IFK_LOCALHOST);
    n->l3_id = (l3_id_t)(g + 1);
    if (!n->is_localhost && mode != IPV4_CFG_DISABLED) {
        n->routing_table = ipv4_rt_create(n->l3_id);
        if (!n->routing_table) {
            memset(&g_v4[g], 0, sizeof(g_v4[g]));
            return 0;
        }
        ipv4_rt_ensure_basics((ipv4_rt_table_t*)n->routing_table, n->ip, n->mask, n->gw, l2->base_metric);
    }

    n->epoch = net_interface_mark_changed();
    l2->l3_v4[loc] = n;
    g_v4[g].used = true;
    l2->ipv4_count++;


    if (!had_active_v4 && l2->kind != NET_IFK_LOCALHOST && l2_has_active_v4(l2)) {
        for (int i = 0; i < (int)l2->ipv4_mcast_count; i++) igmp_send_join(l2->ifindex, l2->ipv4_mcast[i]);
    }

    return n->l3_id;
}

bool l3_ipv4_update(l3_id_t l3_id, uint32_t ip, uint32_t mask, uint32_t gw, ipv4_cfg_t mode, net_runtime_opts_t *runtime_opts) {
    l3_ipv4_interface_t *n = l3_ipv4_find_by_id(l3_id);
    if (!n) return false;
    l2_interface_t *l2 = n->l2;
    if (!l2) return false;
    bool had_active_v4 = l2_has_active_v4(l2);
    if (mode == IPV4_CFG_DHCP && n->mode != IPV4_CFG_DHCP) {
        if (v4_has_dhcp_on_l2(l2->ifindex)) return false;
    }
    if (mode == IPV4_CFG_STATIC){
        if (ipv4_is_unspecified(ip)) return false;
        if (!ipv4_mask_is_contiguous(mask)) return false;
        if (ipv4_is_loopback(ip)&& (l2->kind != NET_IFK_LOCALHOST)) return false;
        if (ipv4_is_multicast(ip)) return false;
        if (ipv4_is_reserved_special(ip)) {
            if (!(ipv4_is_loopback(ip) && l2->kind == NET_IFK_LOCALHOST)) return false;
        }
        if (ipv4_is_network_address(ip, mask)) return false;
        if (ipv4_is_broadcast_address(ip, mask)) return false;
        if (ip != n->ip && v4_ip_exists_anywhere(ip)) return false;
        for (int i = 0; i < MAX_IPV4_L3_INTERFACES; i++){
            if (!g_v4[i].used) continue;
            l3_ipv4_interface_t *x = &g_v4[i].node;
            if (x==n) continue;
            if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
            if (x->mode == IPV4_CFG_DISABLED) continue;
            uint32_t m = (x->mask < mask) ? x->mask : mask;
            if (ipv4_net(ip, m) != ipv4_net(x->ip, m)) continue;
            if (x->mask == mask && ipv4_net(ip, mask) == ipv4_net(x->ip, x->mask)) continue;
            return false;
        }
        if (ip != n->ip && l2->kind != NET_IFK_LOCALHOST && !arp_dad_ipv4_on(l2->ifindex, ip)) return false;
    }

    uint32_t old_ip = n->ip;
    uint32_t old_mask = n->mask;
    bool needs_route = !n->is_localhost && mode != IPV4_CFG_DISABLED;
    ipv4_rt_table_t *new_rt = NULL;
    if (needs_route && !n->routing_table) {
        new_rt = ipv4_rt_create(n->l3_id);
        if (!new_rt) return false;
    }

    bool l3_changed = n->mode != mode || n->gw != gw;
    if (mode == IPV4_CFG_STATIC || mode == IPV4_CFG_DHCP) {
        if (n->ip != ip || n->mask != mask) l3_changed = true;
    } else if (n->ip || n->mask) {
        l3_changed = true;
    }

    n->mode = mode;

    if (runtime_opts) n->runtime_opts_v4 = *runtime_opts;

    if (mode == IPV4_CFG_STATIC || mode == IPV4_CFG_DHCP) {
        n->ip = ip;
        n->mask = mask;
        n->gw = gw;
        n->broadcast = ipv4_broadcast_calc(ip, mask);
    } else {
        n->ip = 0;
        n->mask = 0;
        n->gw = 0;
        n->broadcast = 0;
    }

    if (needs_route) {
        if (new_rt) n->routing_table = new_rt;
        if (old_ip && old_mask) {
            uint32_t old_net = old_ip & old_mask;
            uint32_t new_net = (n->ip && n->mask) ? (n->ip & n->mask) : 0;
            if (!n->ip || !n->mask || old_mask != n->mask || old_net != new_net) {
                ipv4_rt_del_in((ipv4_rt_table_t*)n->routing_table, old_net, old_mask);
            }
        }
        ipv4_rt_sync_basics((ipv4_rt_table_t*)n->routing_table, n->ip, n->mask, n->gw, l2->base_metric);
    } else if (n->routing_table) {
        ipv4_rt_destroy((ipv4_rt_table_t*)n->routing_table);
        n->routing_table = NULL;
    }
    if (!had_active_v4 && l2->kind != NET_IFK_LOCALHOST && l2_has_active_v4(l2)) {
        for (int i = 0; i < (int)l2->ipv4_mcast_count; i++) igmp_send_join(l2->ifindex, l2->ipv4_mcast[i]);
    }

    if (l3_changed) n->epoch = net_interface_mark_changed();
    return true;
}

bool l3_ipv4_remove_from_interface(l3_id_t l3_id) {
    l3_ipv4_interface_t *n = l3_ipv4_find_by_id(l3_id);
    if (!n) return false;
    l2_interface_t *l2 = n->l2;
    if (!l2) return false;
    if (l2->ipv4_count <= 1) return false;

    int g = -1;
    for (int i=0;i<MAX_IPV4_L3_INTERFACES;i++){
        if (g_v4[i].used && &g_v4[i].node == n){ g = i; break; }
    }
    if (g < 0) return false;


    for (int slot = 0; slot < MAX_IPV4_PER_INTERFACE; slot++) {
        if (l2->l3_v4[slot] != n) continue;
        l2->l3_v4[slot] = NULL;
        if (l2->ipv4_count) l2->ipv4_count--;
        net_interface_mark_changed();
        break;
    }

    if (n->routing_table) {
        ipv4_rt_destroy((ipv4_rt_table_t*)n->routing_table);
        n->routing_table = 0;
    }

    g_v4[g].used = false;
    memset(&g_v4[g], 0, sizeof(g_v4[g]));
    return true;
}

l3_ipv4_interface_t* l3_ipv4_find_by_id(l3_id_t l3_id) {
    if (!l3_id || l3_id > MAX_IPV4_L3_INTERFACES) return NULL;
    v4_slot_t *slot = &g_v4[l3_id - 1];
    return slot->used ? &slot->node : NULL;
}
l3_ipv4_interface_t* l3_ipv4_find_by_ip(uint32_t ip){
    for (int i=0;i<MAX_IPV4_L3_INTERFACES;i++){ if (g_v4[i].used && g_v4[i].node.ip == ip) return &g_v4[i].node; }
    return NULL;
}

l3_id_t l3_ipv6_add_to_interface(uint8_t ifindex, const uint8_t ip[16], uint8_t prefix_len, const uint8_t gw[16], ipv6_cfg_t cfg, uint8_t kind) {
    l2_interface_t *l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return 0;
    bool had_active_v6 = l2_has_active_v6(l2);
    uint8_t pre_mcast_count = l2->ipv6_mcast_count;
    if (prefix_len > 128) return 0;

    int placeholder_ll = 0;
    if (ip[0]==0xFE && ip[1]==0x80) {
        placeholder_ll = 1;
        for(int i_ = 2; i_ < 16; i_++) {
            if (ip[i_] != 0) {
                placeholder_ll=0; break;
            }
        }
    }
    int placeholder_gua = 0;
    if (ip[0]==0x20 && ip[1]==0x00) {
        placeholder_gua = 1;
        for(int i_=2;i_<16;i_++) {
            if (ip[i_]!=0) {
                placeholder_gua=0;
                break;
            }
        }
    }

    if (kind & IPV6_ADDRK_LINK_LOCAL){
        if (!(cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))){
            if (!ipv6_is_linklocal(ip)) return 0;
        }
        if (!ipv6_is_unspecified(ip) && !placeholder_ll && v6_ip_exists_anywhere(ip)) return 0;
        for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
            if (!g_v6[i].used) continue;
            if (!g_v6[i].node.l2 || g_v6[i].node.l2->ifindex != ifindex) continue;
            if (ipv6_is_linklocal(g_v6[i].node.ip) && g_v6[i].node.cfg != IPV6_CFG_DISABLE) return 0;
        }
    } else if (kind & IPV6_ADDRK_GLOBAL){
        int is_loop = ipv6_is_loopback(ip);

        if (!(cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))){
            if (ipv6_is_unspecified(ip)) return 0;
        }
        if (!ipv6_is_unspecified(ip)){
            if (ipv6_is_multicast(ip)) return 0;
            if (is_loop && (l2->kind != NET_IFK_LOCALHOST)) return 0;
            if (!is_loop){
                if (ipv6_is_ula(ip)) return 0;
                if (!placeholder_gua){
                    if (v6_ip_exists_anywhere(ip)) return 0;
                    if (v6_overlap_intra_l2(ifindex, ip, prefix_len, NULL)) return 0;
                }
            }
        }
        if (!is_loop){
            bool has_lla=false;
            for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
                if (!g_v6[i].used) continue;
                l3_ipv6_interface_t *x=&g_v6[i].node;
                if (!x->l2 || x->l2->ifindex != ifindex) continue;
                if (ipv6_is_linklocal(x->ip) && x->cfg != IPV6_CFG_DISABLE){ has_lla=true; break; }
            }
            if (!has_lla) return 0;
        }
    } else {
        return 0;
    }

    if (l2->ipv6_count >= MAX_IPV6_PER_INTERFACE) return 0;

    int loc = -1;
    for (int s=0; s<MAX_IPV6_PER_INTERFACE; s++) if (l2->l3_v6[s] == NULL) {
        loc = s;
        break;
    }

    int g = -1;
    for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++) if (!g_v6[i].used) {
        g = i;
        break;
    }
    if (loc < 0 || g < 0) return 0;

    memset(&g_v6[g], 0, sizeof(g_v6[g]));
    l3_ipv6_interface_t *n = &g_v6[g].node;
    n->l2 = l2;
    n->cfg = cfg;
    n->kind = kind;
    n->mtu = 0;

    uint8_t final_ip[16];
    ipv6_cpy(final_ip, ip);
    if ((kind & IPV6_ADDRK_LINK_LOCAL) && placeholder_ll){
        ipv6_make_lla_from_mac(ifindex, final_ip);
        prefix_len = 64;
    }

    ipv6_cpy(n->ip, final_ip);
    n->prefix_len = prefix_len;
    ipv6_cpy(n->gateway, gw);
    n->is_localhost = (l2->kind == NET_IFK_LOCALHOST);
    n->valid_lifetime = 0;
    n->preferred_lifetime = 0;
    n->timestamp_created = 0;
    memset(n->prefix, 0, sizeof(n->prefix));
    memset(n->interface_id, 0, sizeof(n->interface_id));
    n->dad_probes_sent = 0;
    n->dad_timer_ms = 0;

    if (n->is_localhost) {
        n->dad_state = IPV6_DAD_OK;
        n->dad_requested = 0;
    } else {
        if (!ipv6_is_unspecified(n->ip) && !ipv6_is_multicast(n->ip) && !ipv6_is_placeholder_gua(n->ip)) {
            n->dad_state = IPV6_DAD_NONE;
            n->dad_requested = 1;
        } else {
            n->dad_state = IPV6_DAD_NONE;
            n->dad_requested = 0;
        }
    }

    n->l3_id = (l3_id_t)(MAX_IPV4_L3_INTERFACES + g + 1);
    if (!n->is_localhost && cfg != IPV6_CFG_DISABLE) {
        n->routing_table = ipv6_rt_create(n->l3_id);
        if (!n->routing_table) {
            memset(&g_v6[g], 0, sizeof(g_v6[g]));
            return 0;
        }
        ipv6_rt_ensure_basics((ipv6_rt_table_t*)n->routing_table, n->ip, n->prefix_len, n->gateway, l2->base_metric);
    }

    n->epoch = net_interface_mark_changed();
    l2->l3_v6[loc] = n;
    g_v6[g].used = true;
    l2->ipv6_count++;

    if (!n->is_localhost && n->cfg != IPV6_CFG_DISABLE && !ipv6_is_unspecified(n->ip) && !ipv6_is_placeholder_gua(n->ip)) {
        uint8_t m[16];
        ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, n->ip, m);
        (void)l2_ipv6_mcast_join(ifindex, m);
    }
    if (!had_active_v6 && l2->kind != NET_IFK_LOCALHOST && l2_has_active_v6(l2)) {
        for (int i = 0; i < (int)pre_mcast_count && i < (int)l2->ipv6_mcast_count; i++) mld_send_join(l2->ifindex, l2->ipv6_mcast[i]);
    }

    return n->l3_id;
}

bool l3_ipv6_update(l3_id_t l3_id, const uint8_t ip[16], uint8_t prefix_len, const uint8_t gw[16], ipv6_cfg_t cfg, uint8_t kind) {
    l3_ipv6_interface_t *n = l3_ipv6_find_by_id(l3_id);
    if (!n) return false;
    l2_interface_t *l2 = n->l2;
    if (!l2) return false;
    bool had_active_v6 = l2_has_active_v6(l2);
    uint8_t pre_mcast_count = l2->ipv6_mcast_count;
    if (prefix_len > 128) return false;

    if (kind == n->kind && cfg == n->cfg && prefix_len == n->prefix_len && ipv6_cmp(ip, n->ip) == 0 && ipv6_cmp(gw, n->gateway) == 0) return true;
    bool l3_changed = kind != n->kind || cfg != n->cfg || prefix_len != n->prefix_len || ipv6_cmp(ip, n->ip) != 0;

    if ((n->kind & IPV6_ADDRK_LINK_LOCAL) && cfg == IPV6_CFG_DISABLE){
        for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
            if (!g_v6[i].used) continue;
            l3_ipv6_interface_t *x = &g_v6[i].node;
            if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
            if ((x->kind & IPV6_ADDRK_GLOBAL) && x->cfg != IPV6_CFG_DISABLE) return false;
        }
    }

    if (kind & IPV6_ADDRK_LINK_LOCAL){
        if (!(cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))){
            if (!ipv6_is_linklocal(ip)) return false;
        }
        if (!ipv6_is_unspecified(ip) && ipv6_cmp(ip, n->ip)!=0 && v6_ip_exists_anywhere(ip)) return false;
        for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
            if (!g_v6[i].used) continue;
            l3_ipv6_interface_t *x=&g_v6[i].node;
            if (x==n) continue;
            if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
            if (ipv6_is_linklocal(x->ip) && x->cfg != IPV6_CFG_DISABLE) return false;
        }
    } else if (kind & IPV6_ADDRK_GLOBAL){
        if (!(cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))){
            if (ipv6_is_unspecified(ip)) return false;
        }
        if (!ipv6_is_unspecified(ip)){
            if (ipv6_is_multicast(ip)) return false;
            if (ipv6_is_loopback(ip) && (l2->kind != NET_IFK_LOCALHOST)) return false;
            if (ipv6_cmp(ip,n->ip)!=0 && v6_ip_exists_anywhere(ip)) return false;
            if (v6_overlap_intra_l2(l2->ifindex, ip, prefix_len, n)) return false;
        }
    } else {
        return false;
    }

    uint8_t old_ip[16];
    uint8_t old_gateway[16];
    ipv6_cpy(old_ip, n->ip);
    ipv6_cpy(old_gateway, n->gateway);
    uint8_t old_prefix_len = n->prefix_len;
    ipv6_cfg_t old_cfg = n->cfg;
    bool needs_route = !n->is_localhost && cfg != IPV6_CFG_DISABLE;
    ipv6_rt_table_t *new_rt = NULL;
    if (needs_route && !n->routing_table) {
        new_rt = ipv6_rt_create(n->l3_id);
        if (!new_rt) return false;
    }

    if (ipv6_cmp(old_gateway, gw) != 0) l3_changed = true;
    bool old_has_sn = !n->is_localhost && old_cfg != IPV6_CFG_DISABLE && !ipv6_is_unspecified(old_ip) && !ipv6_is_placeholder_gua(old_ip);
    uint8_t old_sn[16] = {0};
    if (old_has_sn) ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, old_ip, old_sn);

    n->cfg = cfg;
    n->kind = kind;
    ipv6_cpy(n->ip, ip);
    n->prefix_len = prefix_len;
    ipv6_cpy(n->gateway, gw);

    bool new_has_sn = !n->is_localhost && n->cfg != IPV6_CFG_DISABLE && !ipv6_is_unspecified(n->ip) && !ipv6_is_placeholder_gua(n->ip);
    uint8_t new_sn[16] = {0};
    if (new_has_sn) ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, n->ip, new_sn);

    bool same_sn = old_has_sn && new_has_sn && ipv6_cmp(old_sn, new_sn) == 0;
    if (old_has_sn && !same_sn) (void)l2_ipv6_mcast_leave(l2->ifindex, old_sn);
    if (new_has_sn && !same_sn) (void)l2_ipv6_mcast_join(l2->ifindex, new_sn);
    
    if (ipv6_cmp(old_ip, n->ip) != 0 || (old_cfg == IPV6_CFG_DISABLE) != (n->cfg == IPV6_CFG_DISABLE)) {
        n->dad_state = IPV6_DAD_NONE;
        n->dad_timer_ms = 0;
        n->dad_probes_sent = 0;

        if (n->is_localhost) {
            n->dad_requested = 0;
            n->dad_state = IPV6_DAD_OK;
        } else if (ipv6_is_unspecified(n->ip) || ipv6_is_multicast(n->ip) || n->cfg == IPV6_CFG_DISABLE) {
            n->dad_requested = 0;
        } else {
            n->dad_requested = 1;
        }
    }

    if (!n->is_localhost) {
        if (needs_route) {
            if (new_rt) n->routing_table = new_rt;
            if (old_prefix_len && !ipv6_is_unspecified(old_ip)) {
                uint8_t old_net[16];
                uint8_t new_net[16];
                ipv6_prefix_network(old_ip, old_prefix_len, old_net);
                if (n->prefix_len && !ipv6_is_unspecified(n->ip)) ipv6_prefix_network(n->ip, n->prefix_len, new_net);
                if (!n->prefix_len || ipv6_is_unspecified(n->ip) || old_prefix_len != n->prefix_len || ipv6_cmp(old_net, new_net) != 0) {
                    ipv6_rt_del_in((ipv6_rt_table_t*)n->routing_table, old_net, old_prefix_len);
                }
            }
            ipv6_rt_sync_basics((ipv6_rt_table_t*)n->routing_table, n->ip, n->prefix_len, n->gateway, l2->base_metric);
        } else if (n->routing_table) {
            ipv6_rt_destroy((ipv6_rt_table_t*)n->routing_table);
            n->routing_table = NULL;
        }
    } else if (n->routing_table) {
        ipv6_rt_destroy((ipv6_rt_table_t*)n->routing_table);
        n->routing_table = NULL;
    }
    if (!had_active_v6 && l2->kind != NET_IFK_LOCALHOST && l2_has_active_v6(l2)) {
        for (int i = 0; i < (int)pre_mcast_count && i < (int)l2->ipv6_mcast_count; i++) mld_send_join(l2->ifindex, l2->ipv6_mcast[i]);
    }

    if (l3_changed) n->epoch = net_interface_mark_changed();
    return true;
}

bool l3_ipv6_remove_from_interface(l3_id_t l3_id) {
    l3_ipv6_interface_t *n = l3_ipv6_find_by_id(l3_id);
    if (!n) return false;
    l2_interface_t *l2 = n->l2;
    if (!l2) return false;
    if ((n->kind & IPV6_ADDRK_LINK_LOCAL)){
        for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
            if (!g_v6[i].used) continue;
            l3_ipv6_interface_t *x=&g_v6[i].node;
            if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
            if ((x->kind & IPV6_ADDRK_GLOBAL) && x->cfg != IPV6_CFG_DISABLE) return false;
        }
    }
    if (l2->ipv6_count <= 1) return false;

    int g = -1;
    for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
        if (g_v6[i].used && &g_v6[i].node == n){ g = i; break; }
    }
    if (g < 0) return false;

    if (!n->is_localhost && n->cfg != IPV6_CFG_DISABLE && !ipv6_is_unspecified(n->ip) && !ipv6_is_placeholder_gua(n->ip)) {
        uint8_t sn[16];
        ipv6_make_multicast(2, IPV6_MCAST_SOLICITED_NODE, n->ip, sn);
        (void)l2_ipv6_mcast_leave(l2->ifindex, sn);
    }

    for (int slot = 0; slot < MAX_IPV6_PER_INTERFACE; slot++) {
        if (l2->l3_v6[slot] != n) continue;
        l2->l3_v6[slot] = NULL;
        if (l2->ipv6_count) l2->ipv6_count--;
        net_interface_mark_changed();
        break;
    }

    if (n->routing_table){
        ipv6_rt_destroy((ipv6_rt_table_t*)n->routing_table);
        n->routing_table = NULL;
    }

    g_v6[g].used = false;
    memset(&g_v6[g], 0, sizeof(g_v6[g]));
    return true;
}

bool l3_ipv6_set_enabled(l3_id_t l3_id, bool enable) {
    l3_ipv6_interface_t *n = l3_ipv6_find_by_id(l3_id);
    if (!n) return false;
    if (enable) return true;

    if (n->kind & IPV6_ADDRK_LINK_LOCAL) {
        l2_interface_t *l2 = n->l2;
        for (int i = 0; i < MAX_IPV6_L3_INTERFACES; i++) {
            if (!g_v6[i].used) continue;
            l3_ipv6_interface_t *x = &g_v6[i].node;
            if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
            if ((x->kind & IPV6_ADDRK_GLOBAL) && x->cfg != IPV6_CFG_DISABLE) return false;
        }
    }
    n->cfg = IPV6_CFG_DISABLE;
    n->dad_state = IPV6_DAD_NONE;
    n->dad_probes_sent = 0;
    n->dad_timer_ms = 0;
    n->epoch = net_interface_mark_changed();
    return true;
}

l3_ipv6_interface_t* l3_ipv6_find_by_id(l3_id_t l3_id) {
    if (l3_id <= MAX_IPV4_L3_INTERFACES || l3_id > MAX_L3_INTERFACES) return NULL;
    v6_slot_t *slot = &g_v6[l3_id - MAX_IPV4_L3_INTERFACES - 1];
    return slot->used ? &slot->node : NULL;
}
l3_ipv6_interface_t* l3_ipv6_find_by_ip(const uint8_t ip[16]){
    for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
        if (g_v6[i].used && ipv6_cmp(g_v6[i].node.ip, ip)==0) return &g_v6[i].node;
    }
    return NULL;
}

void l3_init_localhost_ipv4(void){
    l2_interface_t *lo = NULL;
    for (int i=0;i<(int)MAX_L2_INTERFACES;i++){
        if (!g_l2_used[i]) continue;
        if (g_l2[i].kind == NET_IFK_LOCALHOST) {
            lo = &g_l2[i];
            break;
        }
    }
    if (!lo) return;
    for (int i=0;i<MAX_IPV4_L3_INTERFACES;i++){
        if (!g_v4[i].used) continue;
        if (!g_v4[i].node.l2 || g_v4[i].node.l2 != lo) continue;
        if (ipv4_is_loopback(g_v4[i].node.ip)) return;
    }
    (void)l3_ipv4_add_to_interface(lo->ifindex, 0x7F000001u, 0xFF000000u, 0, IPV4_CFG_STATIC, NULL);
}

void l3_init_localhost_ipv6(void){
    l2_interface_t *lo = NULL;
    for (int i=0;i<(int)MAX_L2_INTERFACES;i++){
        if (!g_l2_used[i]) continue;
        if (g_l2[i].kind == NET_IFK_LOCALHOST) {
            lo = &g_l2[i];
            break;
        }
    }
    if (!lo) return;
    uint8_t loop6[16]={0}; loop6[15]=1;
    for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
        if (!g_v6[i].used) continue;
        if (!g_v6[i].node.l2 || g_v6[i].node.l2 != lo) continue;
        if (ipv6_is_loopback(g_v6[i].node.ip)) return;
    }
    (void)l3_ipv6_add_to_interface(lo->ifindex, loop6, 128, (const uint8_t[16]){0}, IPV6_CFG_STATIC, IPV6_ADDRK_GLOBAL);
}

//TODO: add autoconfig settings/policy
void ifmgr_autoconfig_l2(uint8_t ifindex){
    l2_interface_t *l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;

    if (l2->kind == NET_IFK_LOCALHOST) return;

    if (l2->ipv4_count == 0){
        (void)l3_ipv4_add_to_interface(ifindex, 0, 0, 0, IPV4_CFG_DHCP, NULL);
    }

    bool has_lla=false;
    bool has_gua=false;

    for (int i=0;i<MAX_IPV6_L3_INTERFACES;i++){
        if (!g_v6[i].used) continue;

        l3_ipv6_interface_t *x=&g_v6[i].node;
        if (!x->l2 || x->l2->ifindex != ifindex) continue;

        if (!has_lla && ipv6_is_linklocal(x->ip) && x->cfg != IPV6_CFG_DISABLE) has_lla = true;

        if (!has_gua) {
            if ((x->kind == IPV6_ADDRK_GLOBAL) && x->cfg != IPV6_CFG_DISABLE) has_gua=true;
            else if ((x->kind == IPV6_ADDRK_GLOBAL) && ipv6_is_placeholder_gua(x->ip)) has_gua=true;
        }

        if (has_lla && has_gua) break;
    }
    if (!has_lla){
        uint8_t lla[16];

        ipv6_make_lla_from_mac(ifindex, lla);
        (void)l3_ipv6_add_to_interface(ifindex, lla, 64, (const uint8_t[16]){0}, IPV6_CFG_SLAAC, IPV6_ADDRK_LINK_LOCAL);

    }

    if (!has_gua) {
        uint8_t ph[16];

        ipv6_make_placeholder_gua(ph);
        (void)l3_ipv6_add_to_interface(ifindex, ph, 64, (const uint8_t[16]){0}, IPV6_CFG_STATELESS, IPV6_ADDRK_GLOBAL);
    }
}

void ifmgr_autoconfig_all_l2(void){
    for (int i=0;i<(int)MAX_L2_INTERFACES;i++){
        if (!g_l2_used[i]) continue;
        ifmgr_autoconfig_l2(g_l2[i].ifindex);
    }
}

ip_resolution_result_t resolve_ipv4_to_interface(uint32_t dst_ip) {
    ip_resolution_result_t r = {0};
    l3_id_t chosen = 0;
    if (!ipv4_rt_pick_best_l3(dst_ip, 0, &chosen)) return r;

    l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(chosen);
    if (!ipv4_l3_is_ready(v4)) return r;

    r.found = true;
    r.ipv4 = v4;
    r.l2 = v4->l2;
    return r;
}

ip_resolution_result_t resolve_ipv6_to_interface(const uint8_t dst_ip[16]) {
    ip_resolution_result_t r = {0};
    if (!dst_ip) return r;

    l3_id_t chosen = 0;
    if (!ipv6_rt_pick_best_l3(dst_ip, 0, &chosen)) return r;

    l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(chosen);
    if (!ipv6_l3_is_ready(v6)) return r;

    r.found = true;
    r.ipv6 = v6;
    r.l2 = v6->l2;
    return r;
}