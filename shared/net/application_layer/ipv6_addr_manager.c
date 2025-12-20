#include "ipv6_addr_manager.h"
#include "networking/interface_manager.h"
#include "net/link_layer/ndp.h"
#include "net/internet_layer/ipv6_utils.h"
#include "syscalls/syscalls.h"
#include "process/scheduler.h"
#include "std/memory.h"
#include "math/rng.h"
#include "types.h"

static rng_t g_rng;

static void make_random_iid(uint8_t out_iid[8]) {
    uint64_t x = 0;

    do x = rng_next64(&g_rng);
    while (x == 0);

    out_iid[0] = (uint8_t)((x >> 56) & 0xFF);
    out_iid[1] = (uint8_t)((x >> 48) & 0xFF);
    out_iid[2] = (uint8_t)((x >> 40) & 0xFF);
    out_iid[3] = (uint8_t)((x >> 32) & 0xFF);
    out_iid[4] = (uint8_t)((x >> 24) & 0xFF);
    out_iid[5] = (uint8_t)((x >> 16) & 0xFF);
    out_iid[6] = (uint8_t)((x >> 8) & 0xFF);
    out_iid[7] = (uint8_t)(x & 0xFF);
}

static int l2_is_localhost(const l2_interface_t* l2) {
    if (!l2) return 0;

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (v6 && v6->is_localhost) return 1;
    }

    for (int i = 0; i < MAX_IPV4_PER_INTERFACE; i++) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[i];
        if (v4 && v4->is_localhost) return 1;
    }

    return 0;
}

void ipv6_addr_manager_on_ra(uint8_t ifindex, const uint8_t router_ip[16], uint16_t router_lifetime, const uint8_t prefix[16], uint8_t prefix_len, uint32_t valid_lft, uint32_t preferred_lft, uint8_t autonomous) {
    if (!ifindex) return;
    if (prefix_len != 64) return;
    if (ipv6_is_unspecified(prefix) || ipv6_is_multicast(prefix) || ipv6_is_linklocal(prefix)) return;

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return;

    uint32_t now_ms = get_time();
    uint8_t zero16[16] = {0};
    l3_ipv6_interface_t* slot = NULL;

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (!(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
        if (v6->cfg != IPV6_CFG_SLAAC) continue;

        if (memcmp(v6->prefix, zero16, 16) != 0) {
            if (ipv6_common_prefix_len(v6->prefix, prefix) >= 64) {
                slot = v6;
                break;
            }
        } else {
            if (ipv6_is_placeholder_gua(v6->ip)) {
                slot = v6;
                break;
            }
            if (!ipv6_is_unspecified(v6->ip) && !ipv6_is_multicast(v6->ip) && !ipv6_is_linklocal(v6->ip)) {
                if (ipv6_common_prefix_len(v6->ip, prefix) >= 64) {
                    slot = v6;
                    break;
                }
            }
        }
    }

    if (!slot) {
        uint8_t ph[16];
        ipv6_make_placeholder_gua(ph);

        uint8_t id = l3_ipv6_add_to_interface(ifindex, ph, 64, zero16, IPV6_CFG_SLAAC, IPV6_ADDRK_GLOBAL);
        if (!id) return;

        slot = l3_ipv6_find_by_id(id);
        if (!slot) return;
    }

    slot->ra_has = 1;
    slot->ra_autonomous = autonomous ? 1 : 0;
    slot->ra_is_default = router_lifetime != 0;
    slot->ra_last_update_ms = now_ms;

    ipv6_cpy(slot->prefix, prefix);

    if (slot->ra_is_default && router_ip) ipv6_cpy(slot->gateway, router_ip);
    else ipv6_cpy(slot->gateway, zero16);

    slot->valid_lifetime = valid_lft;
    slot->preferred_lifetime = preferred_lft;

    if (memcmp(slot->ip, zero16, 16) == 0) slot->timestamp_created = now_ms;

    if (!ipv6_is_placeholder_gua(slot->ip) && !ipv6_is_unspecified(slot->ip)) memcpy(slot->interface_id, slot->ip + 8, 8);
}

static void ensure_linklocal(uint8_t ifindex, l2_interface_t* l2) {
    int has_lla = 0;
    l3_ipv6_interface_t* lla = NULL;

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (ipv6_is_unspecified(v6->ip) || ipv6_is_multicast(v6->ip)) continue;
        if (ipv6_is_linklocal(v6->ip)) { has_lla = 1; lla = v6; break; }
    }

    if (has_lla) {
        if (lla && lla->dad_state == IPV6_DAD_NONE && !lla->dad_requested) (void)ndp_request_dad_on(ifindex, lla->ip);
        return;
    }

    uint8_t lla_ip[16];
    uint8_t zero16[16] = {0};

    ipv6_make_lla_from_mac(ifindex, lla_ip);

    uint8_t id = l3_ipv6_add_to_interface(ifindex, lla_ip, 64, zero16, IPV6_CFG_SLAAC, IPV6_ADDRK_LINK_LOCAL);
    if (!id) return;

    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(id);
    if (v6) (void)ndp_request_dad_on(ifindex, v6->ip);
}

static void handle_dad_failed(l3_ipv6_interface_t* v6) {
    if (!v6) return;

    uint8_t iid[8];
    uint8_t new_ip[16];
    uint8_t zero16[16] = {0};

    make_random_iid(iid);

    if (ipv6_is_linklocal(v6->ip)) {
        new_ip[0] = 0xFE;
        new_ip[1] = 0x80;
        memset(new_ip + 2, 0, 6);
        memcpy(new_ip + 8, iid, 8);
        
        (void)l3_ipv6_update(v6->l3_id, new_ip, 64, zero16, v6->cfg, v6->kind);
        (void)ndp_request_dad_on(v6->l2 ? v6->l2->ifindex : 0, new_ip);
        return;
    }

    if (v6->prefix_len != 64) {
        ipv6_cpy(new_ip, v6->ip);
        memcpy(new_ip + 8, iid, 8);

        (void)l3_ipv6_update(v6->l3_id, new_ip, v6->prefix_len, v6->gateway, v6->cfg, v6->kind);
        (void)ndp_request_dad_on(v6->l2 ? v6->l2->ifindex : 0, new_ip);
        return;
    }

    if (memcmp(v6->prefix, zero16, 16) != 0) ipv6_cpy(new_ip, v6->prefix);
    else {
        ipv6_cpy(new_ip, v6->ip);
        memset(new_ip + 8, 0, 8);
    }

    memcpy(new_ip + 8, iid, 8);

    (void)l3_ipv6_update(v6->l3_id, new_ip, 64, v6->gateway, v6->cfg, v6->kind);
    (void)ndp_request_dad_on(v6->l2 ? v6->l2->ifindex : 0, new_ip);
}

static void handle_lifetimes(uint32_t now_ms, l3_ipv6_interface_t* v6) {
    if (!v6) return;
    if (ipv6_is_placeholder_gua(v6->ip)) return;
    if (ipv6_is_unspecified(v6->ip)) return;
    if (ipv6_is_linklocal(v6->ip)) return;
    if (!v6->ra_last_update_ms) return;

    uint32_t elapsed_ms = now_ms >= v6->ra_last_update_ms ? now_ms - v6->ra_last_update_ms : 0;

    if (v6->preferred_lifetime && v6->preferred_lifetime != 0xFFFFFFFFu) {
        uint64_t pref_ms = (uint64_t)v6->preferred_lifetime * 1000ull;
        if ((uint64_t)elapsed_ms >= pref_ms) v6->preferred_lifetime = 0;
    }

    if (v6->valid_lifetime == 0xFFFFFFFFu) return;

    uint64_t valid_ms = (uint64_t)v6->valid_lifetime * 1000ull;
    if ((uint64_t)elapsed_ms >= valid_ms) if (!l3_ipv6_remove_from_interface(v6->l3_id)) (void)l3_ipv6_set_enabled(v6->l3_id, false);
}

static void apply_ra_policy(uint32_t now_ms, l2_interface_t* l2) {
    if (!l2) return;

    uint8_t ifx = l2->ifindex;
    if (!ifx || ifx > MAX_L2_INTERFACES) return;

    uint8_t zero16[16] = {0};

    for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
        l3_ipv6_interface_t* v6 = l2->l3_v6[i];
        if (!v6) continue;
        if (v6->cfg == IPV6_CFG_DISABLE) continue;
        if (!(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
        if (v6->cfg != IPV6_CFG_SLAAC) continue;
        if (!v6->ra_has) continue;
        if (memcmp(v6->prefix, zero16, 16) == 0) continue;

        if (ipv6_is_placeholder_gua(v6->ip)) {
            uint8_t iid[8];
            uint8_t ip[16];

            make_random_iid(iid);
            ipv6_cpy(ip, v6->prefix);
            memcpy(ip + 8, iid, 8);

            (void)l3_ipv6_update(v6->l3_id, ip, 64, v6->gateway, v6->cfg, v6->kind);

            v6->timestamp_created = now_ms;
            memcpy(v6->interface_id, ip + 8, 8);

            if (v6->dad_state == IPV6_DAD_NONE && !v6->dad_requested) (void)ndp_request_dad_on(ifx, ip);

            continue;
        }

        uint8_t gw[16];
        if (v6->ra_is_default) ipv6_cpy(gw, v6->gateway);
        else memset(gw, 0, 16);

        (void)l3_ipv6_update(v6->l3_id, v6->ip, v6->prefix_len, gw, v6->cfg, v6->kind);

        v6->timestamp_created = now_ms;
        memcpy(v6->interface_id, v6->ip + 8, 8);
    }
}

int ipv6_addr_manager_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    rng_init_random(&g_rng);

    const uint32_t tick_ms = 1000;

    while (1) {
        uint32_t now_ms = get_time();

        uint8_t n = l2_interface_count();
        for (uint8_t i = 0; i < n; i++) {
            l2_interface_t* l2 = l2_interface_at(i);
            if (!l2) continue;
            if (!l2->is_up) continue;
            if (l2_is_localhost(l2)) continue;

            ensure_linklocal(l2->ifindex, l2);
            apply_ra_policy(now_ms, l2);

            for (int s = 0; s < MAX_IPV6_PER_INTERFACE; s++) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[s];
                if (!v6) continue;
                if (v6->cfg == IPV6_CFG_DISABLE) continue;
                if (ipv6_is_unspecified(v6->ip) || ipv6_is_multicast(v6->ip)) continue;

                if (v6->dad_state == IPV6_DAD_FAILED) {
                    handle_dad_failed(v6);
                    continue;
                }

                handle_lifetimes(now_ms, v6);
            }
        }

        sleep(tick_ms);
    }
}