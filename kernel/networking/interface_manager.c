#include "interface_manager.h"
#include "std/memory.h"

static l2_interface_t g_l2[MAX_L2_INTERFACES];
static uint8_t g_l2_used[MAX_L2_INTERFACES];
static uint8_t g_l2_count = 0;

static void copy_name(char dst[16], const char* src) {
    int i = 0;
    if (!src) { dst[0] = 0; return; }
    while (src[i] && i < 15) { dst[i] = src[i]; i += 1; }
    dst[i] = 0;
}

static int find_free_slot() {
    for (int i = 0; i < (int)MAX_L2_INTERFACES; ++i) if (!g_l2_used[i]) return i;
    return -1;
}

uint8_t l2_interface_create(const char *name, void *driver_ctx) {
    int slot = find_free_slot();
    if (slot < 0) return 0;
    l2_interface_t* itf = &g_l2[slot];
    memset(itf, 0, sizeof(*itf));
    itf->ifindex = (uint8_t)(slot + 1);
    copy_name(itf->name, name);
    itf->driver_context = driver_ctx;
    itf->mtu = 1500;
    g_l2_used[slot] = 1;
    g_l2_count += 1;
    return itf->ifindex;
}

bool l2_interface_destroy(uint8_t ifindex) {
    if (!ifindex) return false;
    int slot = (int)ifindex - 1;
    if (slot < 0 || slot >= (int)MAX_L2_INTERFACES) return false;
    if (!g_l2_used[slot]) return false;
    memset(&g_l2[slot], 0, sizeof(l2_interface_t));
    g_l2_used[slot] = 0;
    if (g_l2_count) g_l2_count -= 1;
    return true;
}

l2_interface_t* l2_interface_find_by_index(uint8_t ifindex) {
    if (!ifindex) return 0;
    int slot = (int)ifindex - 1;
    if (slot < 0 || slot >= (int)MAX_L2_INTERFACES) return 0;
    if (!g_l2_used[slot]) return 0;
    return &g_l2[slot];
}

uint8_t l2_interface_count(void) {
    return g_l2_count;
}

l2_interface_t* l2_interface_at(uint8_t idx) {
    uint8_t seen = 0;
    for (int i = 0; i < (int)MAX_L2_INTERFACES; ++i) {
        if (!g_l2_used[i]) continue;
        if (seen == idx) return &g_l2[i];
        seen += 1;
    }
    return 0;
}

bool l2_interface_set_mac(uint8_t ifindex, const uint8_t mac[6]) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    if (!mac) return false;
    itf->mac_addr[0]=mac[0]; itf->mac_addr[1]=mac[1]; itf->mac_addr[2]=mac[2];
    itf->mac_addr[3]=mac[3]; itf->mac_addr[4]=mac[4]; itf->mac_addr[5]=mac[5];
    return true;
}

bool l2_interface_set_mtu(uint8_t ifindex, uint16_t mtu) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    itf->mtu = mtu;
    return true;
}

bool l2_interface_set_up(uint8_t ifindex, bool up) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    itf->is_up = up;
    return true;
}

bool l2_interface_set_send_trampoline(uint8_t ifindex, int (*fn)(struct l2_interface*, const void*, size_t)) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
    itf->send_frame = fn;
    return true;
}

static int find_ipv4_group_index(l2_interface_t* itf, uint32_t group) {
    for (int i = 0; i < (int)itf->ipv4_mcast_count; ++i) {
        if (itf->ipv4_mcast[i] == group) return i;
    }
    return -1;
}

bool l2_ipv4_mcast_join(uint8_t ifindex, uint32_t group) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf) return false;
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
    for (int i = 0; i < (int)itf->ipv6_mcast_count; ++i) {
        int eq = 1;
        for (int j = 0; j < 16; ++j) if (itf->ipv6_mcast[i][j] != group[j]) { eq = 0; break; }
        if (eq) return i;
    }
    return -1;
}

bool l2_ipv6_mcast_join(uint8_t ifindex, const uint8_t group[16]) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf || !group) return false;
    if (find_ipv6_group_index(itf, group) >= 0) return true;
    if (itf->ipv6_mcast_count >= MAX_IPV6_MCAST_PER_INTERFACE) return false;
    for (int j = 0; j < 16; ++j) itf->ipv6_mcast[itf->ipv6_mcast_count][j] = group[j];
    itf->ipv6_mcast_count += 1;
    return true;
}

bool l2_ipv6_mcast_leave(uint8_t ifindex, const uint8_t group[16]) {
    l2_interface_t* itf = l2_interface_find_by_index(ifindex);
    if (!itf || !group) return false;
    int idx = find_ipv6_group_index(itf, group);
    if (idx < 0) return true;
    for (int i = idx + 1; i < (int)itf->ipv6_mcast_count; ++i) {
        for (int j = 0; j < 16; ++j) itf->ipv6_mcast[i-1][j] = itf->ipv6_mcast[i][j];
    }
    if (itf->ipv6_mcast_count) itf->ipv6_mcast_count -= 1;
    return true;
}
