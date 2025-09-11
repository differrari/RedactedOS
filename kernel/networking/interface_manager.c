#include "interface_manager.h"
#include "std/memory.h"

//TODO: add network settings
static inline void mem_zero(void *p, size_t n){ if (p) memset(p,0,n); }
static inline void mem_copy(void *d, const void *s, size_t n){ if (d && s && n) memcpy(d,s,n); }

static void copy_name(char dst[16], const char* src) {
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    while (src[i] && i < 15) { dst[i] = src[i]; i += 1; }
    dst[i] = 0;
}

static inline bool is_power2_mask_contiguous(uint32_t mask){
    if (mask == 0) return true;
    uint32_t inv = ~mask;
    return ((inv & (inv + 1u)) == 0);
}

static inline uint32_t ipv4_net(uint32_t ip, uint32_t mask){ return ip & mask; }
static inline uint32_t ipv4_broadcast_calc(uint32_t ip, uint32_t mask){ return (mask==0)?0:((ip & mask) | ~mask); }

static inline bool ipv4_is_unspecified(uint32_t ip){ return ip == 0; }
static inline bool ipv4_is_loopback(uint32_t ip){ return ((ip & 0xFF000000u) == 0x7F000000u); }
static inline bool ipv4_is_multicast(uint32_t ip){ return ((ip & 0xF0000000u) == 0xE0000000u); }
static inline bool ipv4_is_network_address(uint32_t ip, uint32_t mask){ if (mask==0 || mask==0xFFFFFFFFu) return false; return ((ip & mask) == ip); }
static inline bool ipv4_is_broadcast_address(uint32_t ip, uint32_t mask){ if (mask==0 || mask==0xFFFFFFFFu) return false; return (ip == ((ip & mask) | ~mask)); }
static inline bool ipv4_is_reserved_special(uint32_t ip){
    if ((ip & 0xFF000000u) == 0x00000000u) return true;
    if ((ip & 0xFFFF0000u) == 0xA9FE0000u) return true;
    if ((ip & 0xF0000000u) == 0xF0000000u) return true;
    return false;
}

static inline int prefix_match(const uint8_t a[16], const uint8_t b[16]){
    int bits = 0;
    for (int i=0;i<16;i++){
        uint8_t x = (uint8_t)(a[i] ^ b[i]);
        if (x==0){ bits += 8; continue; }
        for (int bpos=7; bpos>=0; --bpos){
            if (x & (1u<<bpos)) return bits + (7-bpos);
        }
    }
    return 128;
}

static inline bool ipv6_is_unspecified(const uint8_t ip[16]){
    for (int i=0;i<16;i++) if (ip[i]!=0) return false;
    return true;
}
static inline bool ipv6_is_loopback(const uint8_t ip[16]){
    for (int i=0;i<15;i++) if (ip[i]!=0) return false;
    return ip[15]==1;
}
static inline bool ipv6_is_multicast(const uint8_t ip[16]){ return (ip[0]==0xFF); }
static inline bool ipv6_is_ula(const uint8_t ip[16]){ return ((ip[0]&0xFE) == 0xFC); }
static inline bool ipv6_is_linklocal(const uint8_t ip[16]){ return (ip[0]==0xFE && (ip[1]&0xC0)==0x80); }

static inline int cmp16(const uint8_t a[16], const uint8_t b[16]){
    for (int i=0;i<16;i++){ if (a[i]!=b[i]) return (int)a[i] - (int)b[i]; }
    return 0;
}
static inline void cp16(uint8_t dst[16], const uint8_t src[16]){ mem_copy(dst, src, 16); }

static l2_interface_t g_l2[MAX_L2_INTERFACES];
static uint8_t g_l2_used[MAX_L2_INTERFACES];
static uint8_t g_l2_count = 0;

typedef struct {
    l3_ipv4_interface_t node;
    bool used;
    uint8_t slot_in_l2;
} v4_slot_t;
typedef struct {
    l3_ipv6_interface_t node;
    bool used;
    uint8_t slot_in_l2;
} v6_slot_t;

#define V4_POOL_SIZE (MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE)
#define V6_POOL_SIZE (MAX_L2_INTERFACES * MAX_IPV6_PER_INTERFACE)

static v4_slot_t g_v4[V4_POOL_SIZE];
static v6_slot_t g_v6[V6_POOL_SIZE];

static inline int find_free_l2_slot(void){
    for (int i=0;i<(int)MAX_L2_INTERFACES;i++) if (!g_l2_used[i]) return i;
    return -1;
}
static inline int l2_slot_from_ifindex(uint8_t ifindex){
    if (!ifindex) return -1;
    int s = (int)ifindex - 1;
    if (s<0 || s>=(int)MAX_L2_INTERFACES) return -1;
    if (!g_l2_used[s]) return -1;
    return s;
}
static inline uint8_t make_l3_id(uint8_t ifindex, uint8_t local_slot){ return (uint8_t)((ifindex<<4) | (local_slot & 0x0F)); }
static inline uint8_t l3_ifindex_from_id(uint8_t l3_id){ return (uint8_t)((l3_id >> 4) & 0x0F); }
static inline uint8_t l3_local_slot_from_id(uint8_t l3_id){ return (uint8_t)(l3_id & 0x0F); }

static bool v4_has_dhcp_on_l2(uint8_t ifindex){
    for (int i = 0; i < V4_POOL_SIZE; i++){
        if (!g_v4[i].used) continue;
        l3_ipv4_interface_t *x = &g_v4[i].node;
        if (!x->l2) continue;
        if (x->l2->ifindex != ifindex) continue;
        if (x->mode == IPV4_CFG_DHCP) return true;
    }
    return false;
}

uint8_t l2_interface_create(const char *name, void *driver_ctx) {
    int slot = find_free_l2_slot();
    if (slot < 0) return 0;
    l2_interface_t* itf = &g_l2[slot];
    mem_zero(itf, sizeof(*itf));
    itf->ifindex = (uint8_t)(slot + 1);
    copy_name(itf->name, name);
    itf->driver_context = driver_ctx;
    itf->arp_table = NULL;
    itf->nd_table = NULL;
    g_l2_used[slot] = 1;
    g_l2_count += 1;
    return itf->ifindex;
}

bool l2_interface_destroy(uint8_t ifindex) {
    int slot = l2_slot_from_ifindex(ifindex);
    if (slot < 0) return false;
    l2_interface_t* itf = &g_l2[slot];
    if (itf->ipv4_count || itf->ipv6_count) return false;
    mem_zero(&g_l2[slot], sizeof(l2_interface_t));
    g_l2_used[slot] = 0;
    if (g_l2_count) g_l2_count -= 1;
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
    itf->is_up = up;
    return true;
}

static int find_ipv4_group_index(l2_interface_t* itf, uint32_t group) {
    for (int i = 0; i < (int)itf->ipv4_mcast_count; ++i) if (itf->ipv4_mcast[i] == group) return i;
    return -1;
}

bool l2_ipv4_mcast_join(uint8_t ifindex, uint32_t group) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    if (!ipv4_is_multicast(group)) return false;
    if (find_ipv4_group_index(itf, group) >= 0) return true;
    if (itf->ipv4_mcast_count >= MAX_IPV4_MCAST_PER_INTERFACE) return false;
    itf->ipv4_mcast[itf->ipv4_mcast_count++] = group;
    return true;
}

bool l2_ipv4_mcast_leave(uint8_t ifindex, uint32_t group) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    int idx = find_ipv4_group_index(itf, group);
    if (idx < 0) return true;
    for (int i = idx + 1; i < (int)itf->ipv4_mcast_count; ++i) itf->ipv4_mcast[i-1] = itf->ipv4_mcast[i];
    if (itf->ipv4_mcast_count) itf->ipv4_mcast_count -= 1;
    return true;
}

static int find_ipv6_group_index(l2_interface_t* itf, const uint8_t group[16]) {
    for (int i = 0; i < (int)itf->ipv6_mcast_count; ++i) if (cmp16(itf->ipv6_mcast[i], group) == 0) return i;
    return -1;
}
bool l2_ipv6_mcast_join(uint8_t ifindex, const uint8_t group[16]) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf || !group) return false;
    if (!ipv6_is_multicast(group)) return false;
    if (find_ipv6_group_index(itf, group) >= 0) return true;
    if (itf->ipv6_mcast_count >= MAX_IPV6_MCAST_PER_INTERFACE) return false;
    cp16(itf->ipv6_mcast[itf->ipv6_mcast_count], group);
    itf->ipv6_mcast_count += 1;
    return true;
}
bool l2_ipv6_mcast_leave(uint8_t ifindex, const uint8_t group[16]) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf || !group) return false;
    int idx = find_ipv6_group_index(itf, group);
    if (idx < 0) return true;
    for (int i = idx + 1; i < (int)itf->ipv6_mcast_count; ++i) cp16(itf->ipv6_mcast[i-1], itf->ipv6_mcast[i]);
    if (itf->ipv6_mcast_count) itf->ipv6_mcast_count -= 1;
    return true;
}

static bool v4_ip_exists_anywhere(uint32_t ip){
    for (int i=0;i<V4_POOL_SIZE;i++){ if (g_v4[i].used && g_v4[i].node.ip == ip) return true; }
    return false;
}

static bool v4_overlap_intra_l2(uint8_t ifindex, uint32_t ip, uint32_t mask){
    if (!is_power2_mask_contiguous(mask)) return true;
    uint32_t n1 = ipv4_net(ip, mask);
    for (int i=0;i<V4_POOL_SIZE;i++){
        if (!g_v4[i].used) continue;
        l3_ipv4_interface_t *x = &g_v4[i].node;
        if (!x->l2 || x->l2->ifindex != ifindex) continue;
        if (x->mode == IPV4_CFG_DISABLED) continue;
        uint32_t m = (x->mask==0)?mask:((mask==0)?x->mask:((x->mask < mask)?x->mask:mask));
        if (ipv4_net(ip, m) == ipv4_net(x->ip, m)) return true;
        (void)n1;
    }
    return false;
}

static bool v6_ip_exists_anywhere(const uint8_t ip[16]){
    if (ipv6_is_unspecified(ip)) return false;
    for (int i=0;i<V6_POOL_SIZE;i++){ if (g_v6[i].used && cmp16(g_v6[i].node.ip, ip)==0) return true; }
    return false;
}

static bool v6_overlap_intra_l2(uint8_t ifindex, const uint8_t ip[16], uint8_t prefix_len){
    if (ipv6_is_unspecified(ip)) return false;
    for (int i=0;i<V6_POOL_SIZE;i++){
        if (!g_v6[i].used) continue;
        l3_ipv6_interface_t *x = &g_v6[i].node;
        if (!x->l2 || x->l2->ifindex != ifindex) continue;
        if (x->cfg == IPV6_CFG_DISABLE) continue;
        if (ipv6_is_unspecified(x->ip)) continue;
        uint8_t minp = (x->prefix_len < prefix_len) ? x->prefix_len : prefix_len;
        int eq = 1;
        int fb = minp/8, rb = minp%8;
        for (int b=0;b<fb;b++){ if (ip[b]!=x->ip[b]) {eq=0;break;} }
        if (eq && rb){
            uint8_t m=(uint8_t)(0xFF<<(8-rb));
            if ( (ip[fb]&m) != (x->ip[fb]&m) ) eq=0;
        }
        if (eq) return true;
    }
    return false;
}

static int alloc_local_slot_v4(l2_interface_t *l2){
    if (!l2) return -1;
    for (int s=0; s<MAX_IPV4_PER_INTERFACE; s++) if (l2->l3_v4[s] == NULL) return s;
    return -1;
}

static int alloc_local_slot_v6(l2_interface_t *l2){
    if (!l2) return -1;
    for (int s=0; s<MAX_IPV6_PER_INTERFACE; s++) if (l2->l3_v6[s] == NULL) return s;
    return -1;
}

static int alloc_global_v4_slot(void){ for (int i=0;i<V4_POOL_SIZE;i++) if (!g_v4[i].used) return i; return -1; }
static int alloc_global_v6_slot(void){ for (int i=0;i<V6_POOL_SIZE;i++) if (!g_v6[i].used) return i; return -1; }

uint8_t l3_ipv4_add_to_interface(uint8_t ifindex, uint32_t ip, uint32_t mask, uint32_t gw, ipv4_cfg_t mode, net_runtime_opts_t *rt){
    l2_interface_t *l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return 0;

    if (mode == IPV4_CFG_DHCP) {
        if (v4_has_dhcp_on_l2(ifindex)) {
            return 0;
        }
    }

    if (mode == IPV4_CFG_STATIC){
        if (ipv4_is_unspecified(ip)) return 0;
        if (!is_power2_mask_contiguous(mask)) return 0;
        if (ipv4_is_loopback(ip) && !(l2->name[0]=='l' && l2->name[1]=='o')) return 0;
        if (ipv4_is_multicast(ip)) return 0;
        if (ipv4_is_reserved_special(ip)) return 0;
        if (ipv4_is_network_address(ip, mask)) return 0;
        if (ipv4_is_broadcast_address(ip, mask)) return 0;
        if (v4_ip_exists_anywhere(ip)) return 0;
        if (v4_overlap_intra_l2(ifindex, ip, mask)) return 0;
    }
    
    if (l2->ipv4_count >= MAX_IPV4_PER_INTERFACE) return 0;
    int loc = alloc_local_slot_v4(l2);
    int g = alloc_global_v4_slot();
    if (loc < 0 || g < 0) return 0;
    g_v4[g].used = true;
    g_v4[g].slot_in_l2 = (uint8_t)loc;
    l3_ipv4_interface_t *n = &g_v4[g].node;
    mem_zero(n, sizeof(*n));
    n->l2 = l2;
    n->mode = mode;
    n->ip = (mode==IPV4_CFG_STATIC) ? ip : 0;
    n->mask = (mode==IPV4_CFG_STATIC) ? mask : 0;
    n->gw = (mode==IPV4_CFG_STATIC) ? gw : 0;
    n->broadcast = (mode==IPV4_CFG_STATIC) ? ipv4_broadcast_calc(ip, mask) : 0;
    n->rt_v4 = rt;
    n->is_localhost = (l2->name[0]=='l' && l2->name[1]=='o');
    n->l3_id = make_l3_id(l2->ifindex, (uint8_t)loc);
    l2->l3_v4[loc] = n;
    l2->ipv4_count++;
    return n->l3_id;
}

bool l3_ipv4_update(uint8_t l3_id, uint32_t ip, uint32_t mask, uint32_t gw, ipv4_cfg_t mode, net_runtime_opts_t *rt){
    l3_ipv4_interface_t *n = l3_ipv4_find_by_id(l3_id);
    if (!n) return false;

    l2_interface_t *l2 = n->l2;
    if (!l2) return false;

    if (mode == IPV4_CFG_DHCP && n->mode != IPV4_CFG_DHCP) {
        if (v4_has_dhcp_on_l2(l2->ifindex)) {
            return false;
        }
    }

    if (mode == IPV4_CFG_STATIC){
        if (ipv4_is_unspecified(ip)) return false;
        if (!is_power2_mask_contiguous(mask)) return false;
        if (ipv4_is_loopback(ip) && !(l2->name[0]=='l' && l2->name[1]=='o')) return false;
        if (ipv4_is_multicast(ip)) return false;
        if (ipv4_is_reserved_special(ip)) return false;
        if (ipv4_is_network_address(ip, mask)) return false;
        if (ipv4_is_broadcast_address(ip, mask)) return false;
        if (ip != n->ip && v4_ip_exists_anywhere(ip)) return false;

        for (int i = 0; i < V4_POOL_SIZE; i++){
            if (!g_v4[i].used) continue;
            l3_ipv4_interface_t *x = &g_v4[i].node;
            if (x==n) continue;
            if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
            if (x->mode == IPV4_CFG_DISABLED) continue;

            uint32_t m = (x->mask < mask) ? x->mask : mask;
            if (ipv4_net(ip, m) == ipv4_net(x->ip, m)) return false;
        }
    }

    n->mode = mode;
    n->rt_v4 = rt;

    if (mode == IPV4_CFG_STATIC) {
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

    return true;
}


bool l3_ipv4_remove_from_interface(uint8_t l3_id){
    l3_ipv4_interface_t *n = l3_ipv4_find_by_id(l3_id);
    if (!n) return false;
    l2_interface_t *l2 = n->l2;
    if (!l2) return false;
    if (l2->ipv4_count <= 1) return false;
    uint8_t slot = l3_local_slot_from_id(l3_id);
    if (slot < MAX_IPV4_PER_INTERFACE && l2->l3_v4[slot] == n){
        l2->l3_v4[slot] = NULL;
        if (l2->ipv4_count) l2->ipv4_count--;
    }
    for (int i=0;i<V4_POOL_SIZE;i++){
        if (g_v4[i].used && &g_v4[i].node == n){
            g_v4[i].used = false;
            mem_zero(&g_v4[i], sizeof(g_v4[i]));
            break;
        }
    }
    return true;
}

l3_ipv4_interface_t* l3_ipv4_find_by_id(uint8_t l3_id){
    uint8_t ifx = l3_ifindex_from_id(l3_id);
    uint8_t loc = l3_local_slot_from_id(l3_id);
    l2_interface_t *l2 = l2_interface_find_by_index(ifx);
    if (!l2) return NULL;
    if (loc >= MAX_IPV4_PER_INTERFACE) return NULL;
    return l2->l3_v4[loc];
}
l3_ipv4_interface_t* l3_ipv4_find_by_ip(uint32_t ip){
    for (int i=0;i<V4_POOL_SIZE;i++){ if (g_v4[i].used && g_v4[i].node.ip == ip) return &g_v4[i].node; }
    return NULL;
}

uint8_t l3_ipv6_add_to_interface(uint8_t ifindex, const uint8_t ip[16], uint8_t prefix_len, const uint8_t gw[16], ipv6_cfg_t cfg, uint8_t kind){
    l2_interface_t *l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return 0;
    if (prefix_len > 128) return 0;

    int i_;
    int placeholder_ll = 0;
    if (ip[0]==0xFE && ip[1]==0x80){ placeholder_ll = 1; for(i_=2;i_<16;i_++){ if (ip[i_]!=0){ placeholder_ll=0; break; } } }
    int placeholder_gua = 0;
    if (ip[0]==0x20 && ip[1]==0x00){ placeholder_gua = 1; for(i_=2;i_<16;i_++){ if (ip[i_]!=0){ placeholder_gua=0; break; } } }

    if (kind & IPV6_ADDRK_LINK_LOCAL){
        if (!(cfg & (IPV6_CFG_SLAAC | IPV6_CFG_DHCPV6))){
            if (!ipv6_is_linklocal(ip)) return 0;
        }
        if (!ipv6_is_unspecified(ip) && !placeholder_ll && v6_ip_exists_anywhere(ip)) return 0;
        for (int i=0;i<V6_POOL_SIZE;i++){
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
            if (is_loop && !(l2->name[0]=='l' && l2->name[1]=='o')) return 0;
            if (!is_loop){
                if (ipv6_is_ula(ip)) return 0;
                if (!placeholder_gua){
                    if (v6_ip_exists_anywhere(ip)) return 0;
                    if (v6_overlap_intra_l2(ifindex, ip, prefix_len)) return 0;
                }
            }
        }
        if (!is_loop){
            bool has_lla=false;
            for (int i=0;i<V6_POOL_SIZE;i++){
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
    int loc = alloc_local_slot_v6(l2);
    int g = alloc_global_v6_slot();
    if (loc < 0 || g < 0) return 0;

    g_v6[g].used = true;
    g_v6[g].slot_in_l2 = (uint8_t)loc;
    l3_ipv6_interface_t *n = &g_v6[g].node;
    mem_zero(n, sizeof(*n));
    n->l2 = l2;
    n->cfg = cfg;
    n->kind = kind;
    cp16(n->ip, ip);
    n->prefix_len = prefix_len;
    cp16(n->gateway, gw);
    n->is_localhost = (l2->name[0]=='l' && l2->name[1]=='o');
    n->l3_id = make_l3_id(l2->ifindex, (uint8_t)loc);
    l2->l3_v6[loc] = n;
    l2->ipv6_count++;
    return n->l3_id;
}

bool l3_ipv6_update(uint8_t l3_id, const uint8_t ip[16], uint8_t prefix_len, const uint8_t gw[16], ipv6_cfg_t cfg, uint8_t kind){
    l3_ipv6_interface_t *n = l3_ipv6_find_by_id(l3_id);
    if (!n) return false;
    l2_interface_t *l2 = n->l2;
    if (!l2) return false;
    if (prefix_len > 128) return false;

    if ((n->kind & IPV6_ADDRK_LINK_LOCAL) && cfg == IPV6_CFG_DISABLE){
        for (int i=0;i<V6_POOL_SIZE;i++){
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
        if (!ipv6_is_unspecified(ip) && cmp16(ip, n->ip)!=0 && v6_ip_exists_anywhere(ip)) return false;
        for (int i=0;i<V6_POOL_SIZE;i++){
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
            if (ipv6_is_multicast(ip) || ipv6_is_loopback(ip) || ipv6_is_ula(ip)) return false;
            if (cmp16(ip,n->ip)!=0 && v6_ip_exists_anywhere(ip)) return false;
            if (v6_overlap_intra_l2(l2->ifindex, ip, prefix_len)){
                for (int i=0;i<V6_POOL_SIZE;i++){
                    if (!g_v6[i].used) continue;
                    l3_ipv6_interface_t *x=&g_v6[i].node;
                    if (x==n) continue;
                    if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
                    if (ipv6_is_unspecified(x->ip)) continue;
                    uint8_t minp = (x->prefix_len < prefix_len) ? x->prefix_len : prefix_len;
                    int eq = 1;
                    int fb=minp/8, rb=minp%8;
                    for (int b=0;b<fb;b++){ if (ip[b]!=x->ip[b]) {eq=0;break;} }
                    if (eq && rb){
                        uint8_t m=(uint8_t)(0xFF<<(8-rb));
                        if ( (ip[fb]&m) != (x->ip[fb]&m) ) eq=0;
                    }
                    if (eq) return false;
                }
            }
        }
    } else {
        return false;
    }

    n->cfg = cfg;
    n->kind = kind;
    cp16(n->ip, ip);
    n->prefix_len = prefix_len;
    cp16(n->gateway, gw);
    return true;
}

bool l3_ipv6_remove_from_interface(uint8_t l3_id){
    l3_ipv6_interface_t *n = l3_ipv6_find_by_id(l3_id);
    if (!n) return false;
    l2_interface_t *l2 = n->l2;
    if (!l2) return false;
    if ((n->kind & IPV6_ADDRK_LINK_LOCAL)){
        for (int i=0;i<V6_POOL_SIZE;i++){
            if (!g_v6[i].used) continue;
            l3_ipv6_interface_t *x=&g_v6[i].node;
            if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
            if ((x->kind & IPV6_ADDRK_GLOBAL) && x->cfg != IPV6_CFG_DISABLE) return false;
        }
    }
    if (l2->ipv6_count <= 1) return false;
    uint8_t slot = l3_local_slot_from_id(l3_id);
    if (slot < MAX_IPV6_PER_INTERFACE && l2->l3_v6[slot] == n){
        l2->l3_v6[slot] = NULL;
        if (l2->ipv6_count) l2->ipv6_count--;
    }
    for (int i=0;i<V6_POOL_SIZE;i++){
        if (g_v6[i].used && &g_v6[i].node == n){
            g_v6[i].used = false;
            mem_zero(&g_v6[i], sizeof(g_v6[i]));
            break;
        }
    }
    return true;
}

bool l3_ipv6_set_enabled(uint8_t l3_id, bool enable){
    l3_ipv6_interface_t *n = l3_ipv6_find_by_id(l3_id);
    if (!n) return false;
    if (enable){
        if (n->cfg == IPV6_CFG_DISABLE) n->cfg = IPV6_CFG_STATIC;
        return true;
    } else {
        if ((n->kind & IPV6_ADDRK_LINK_LOCAL)){
            l2_interface_t *l2 = n->l2;
            for (int i=0;i<V6_POOL_SIZE;i++){
                if (!g_v6[i].used) continue;
                l3_ipv6_interface_t *x = &g_v6[i].node;
                if (!x->l2 || x->l2->ifindex != l2->ifindex) continue;
                if ((x->kind & IPV6_ADDRK_GLOBAL) && x->cfg != IPV6_CFG_DISABLE) return false;
            }
        }
        n->cfg = IPV6_CFG_DISABLE;
        return true;
    }
}

l3_ipv6_interface_t* l3_ipv6_find_by_id(uint8_t l3_id){
    uint8_t ifx = l3_ifindex_from_id(l3_id);
    uint8_t loc = l3_local_slot_from_id(l3_id);
    l2_interface_t *l2 = l2_interface_find_by_index(ifx);
    if (!l2) return NULL;
    if (loc >= MAX_IPV6_PER_INTERFACE) return NULL;
    return l2->l3_v6[loc];
}
l3_ipv6_interface_t* l3_ipv6_find_by_ip(const uint8_t ip[16]){
    for (int i=0;i<V6_POOL_SIZE;i++){ if (g_v6[i].used && cmp16(g_v6[i].node.ip, ip)==0) return &g_v6[i].node; }
    return NULL;
}

void l3_init_localhost_ipv4(void){
    l2_interface_t *lo = NULL;
    for (int i=0;i<(int)MAX_L2_INTERFACES;i++){
        if (!g_l2_used[i]) continue;
        if (g_l2[i].name[0]=='l' && g_l2[i].name[1]=='o' && g_l2[i].name[2]=='0' && g_l2[i].name[3]==0) { lo = &g_l2[i]; break; }
    }
    if (!lo) return;
    for (int i=0;i<V4_POOL_SIZE;i++){
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
        if (g_l2[i].name[0]=='l' && g_l2[i].name[1]=='o' && g_l2[i].name[2]=='0' && g_l2[i].name[3]==0) { lo = &g_l2[i]; break; }
    }
    if (!lo) return;
    uint8_t loop6[16]={0}; loop6[15]=1;
    for (int i=0;i<V6_POOL_SIZE;i++){
        if (!g_v6[i].used) continue;
        if (!g_v6[i].node.l2 || g_v6[i].node.l2 != lo) continue;
        if (ipv6_is_loopback(g_v6[i].node.ip)) return;
    }
    uint8_t zero16[16]={0};
    (void)l3_ipv6_add_to_interface(lo->ifindex, loop6, 128, zero16, IPV6_CFG_STATIC, IPV6_ADDRK_GLOBAL);
}

void ifmgr_autoconfig_l2(uint8_t ifindex){
    l2_interface_t *l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;

    if (l2->name[0]=='l' && l2->name[1]=='o' && l2->name[2]=='0' && l2->name[3]==0){
        return;
    }

    if (l2->ipv4_count == 0){
        (void)l3_ipv4_add_to_interface(ifindex, 0, 0, 0, IPV4_CFG_DHCP, NULL);
    }

    bool has_lla=false;
    for (int i=0;i<V6_POOL_SIZE;i++){
        if (!g_v6[i].used) continue;
        l3_ipv6_interface_t *x=&g_v6[i].node;
        if (!x->l2 || x->l2->ifindex != ifindex) continue;
        if (ipv6_is_linklocal(x->ip) && x->cfg != IPV6_CFG_DISABLE){ has_lla=true; break; }
    }
    if (!has_lla){
        uint8_t fe80_0[16]={0}; fe80_0[0]=0xFE; fe80_0[1]=0x80;
        uint8_t zero16[16]={0};
        (void)l3_ipv6_add_to_interface(ifindex, fe80_0, 64, zero16, IPV6_CFG_SLAAC, IPV6_ADDRK_LINK_LOCAL);
    }

    bool has_gua=false;
    for (int i=0;i<V6_POOL_SIZE;i++){
        if (!g_v6[i].used) continue;
        l3_ipv6_interface_t *x=&g_v6[i].node;
        if (!x->l2 || x->l2->ifindex != ifindex) continue;
        if (!ipv6_is_linklocal(x->ip) && x->cfg != IPV6_CFG_DISABLE){ has_gua=true; break; }
    }
    if (!has_gua){
        uint8_t g2000_0[16]={0}; g2000_0[0]=0x20; g2000_0[1]=0x00;
        uint8_t zero16[16]={0};
        (void)l3_ipv6_add_to_interface(ifindex, g2000_0, 64, zero16, IPV6_CFG_SLAAC, IPV6_ADDRK_GLOBAL);
    }

    //TODO: add autoconfig settings/policy
}

void ifmgr_autoconfig_all_l2(void){
    for (int i=0;i<(int)MAX_L2_INTERFACES;i++){
        if (!g_l2_used[i]) continue;
        ifmgr_autoconfig_l2(g_l2[i].ifindex);
    }
}

ip_resolution_result_t resolve_ipv4_to_interface(uint32_t dst_ip){
    ip_resolution_result_t r; r.found=false; r.ipv4=NULL; r.ipv6=NULL; r.l2=NULL;
    int best_plen = -1;
    for (int i=0;i<V4_POOL_SIZE;i++){
        if (!g_v4[i].used) continue;
        l3_ipv4_interface_t *x = &g_v4[i].node;
        if (!x->l2) continue;
        if (x->mode == IPV4_CFG_DISABLED) continue;
        uint32_t m = x->mask;
        if (m==0){
            if (x->ip == dst_ip && best_plen < 32){ best_plen = 32; r.found=true; r.ipv4=x; r.l2=x->l2; }
            continue;
        }
        if (ipv4_net(dst_ip, m) == ipv4_net(x->ip, m)){
            int plen=0; uint32_t tmp=m;
            while (tmp){ plen += (tmp & 1u); tmp >>= 1; }
            if (plen > best_plen){ best_plen = plen; r.found=true; r.ipv4=x; r.l2=x->l2; }
        }
    }
    return r;
}

ip_resolution_result_t resolve_ipv6_to_interface(const uint8_t dst_ip[16]){
    ip_resolution_result_t r; r.found=false; r.ipv4=NULL; r.ipv6=NULL; r.l2=NULL;
    int best = -1;
    for (int i=0;i<V6_POOL_SIZE;i++){
        if (!g_v6[i].used) continue;
        l3_ipv6_interface_t *x = &g_v6[i].node;
        if (!x->l2) continue;
        if (x->cfg == IPV6_CFG_DISABLE) continue;
        if (ipv6_is_unspecified(x->ip)) continue;
        int match = prefix_match(dst_ip, x->ip);
        if (match >= x->prefix_len && match > best){ best = match; r.found=true; r.ipv6=x; r.l2=x->l2; }
    }
    return r;
}

bool check_ipv4_overlap(uint32_t new_ip, uint32_t mask, uint8_t ifindex){ return v4_overlap_intra_l2(ifindex, new_ip, mask); }
bool check_ipv6_overlap(const uint8_t new_ip[16], uint8_t prefix_len, uint8_t ifindex){ return v6_overlap_intra_l2(ifindex, new_ip, prefix_len); }
