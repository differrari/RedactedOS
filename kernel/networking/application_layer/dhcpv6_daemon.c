#include "dhcpv6_daemon.h"

#include "std/memory.h"
#include "std/string.h"
#include "syscalls/syscalls.h"
#include "process/scheduler.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"
#include "math/rng.h"
#include "random/random.h"

#include "data/struct/linked_list.h"

#include "networking/interface_manager.h"
#include "networking/link_layer/link_utils.h"
#include "networking/network.h"

#include "networking/application_layer/dhcpv6.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/internet_layer/ipv6_utils.h"

#include "networking/transport_layer/csocket.h"

enum {
    DHCPV6_S_INIT = 0,
    DHCPV6_S_SOLICIT = 1,
    DHCPV6_S_REQUEST = 2,
    DHCPV6_S_BOUND = 3,
    DHCPV6_S_RENEWING = 4,
    DHCPV6_S_REBINDING = 5,
    DHCPV6_S_CONFIRMING = 6,
    DHCPV6_S_RELEASING = 7,
    DHCPV6_S_DECLINING = 8,
    DHCPV6_S_INFO = 9
};

typedef struct {
    uint8_t ifindex;
    l3_id_t target_l3_id;
    l3_id_t bound_linklocal_l3_id;
    uint32_t target_generation;
    uint32_t bound_linklocal_generation;

    uint8_t last_gateway[16];
    uint8_t last_gateway_ok;

    uint8_t mac[MAC_ADDR_LEN];
    uint8_t mac_ok;

    socket_handle_t sock;

    uint32_t xid24;

    uint32_t retry_left_ms;
    uint32_t backoff_ms;

    uint64_t t1_left_ms;
    uint64_t t2_left_ms;
    uint64_t lease_left_ms;
    uint32_t last_tick_ms;

    uint32_t rx_fast_left_ms;
    uint32_t sol_max_rt_ms;
    uint32_t inf_max_rt_ms;
    uint64_t exchange_elapsed_ms;
    uint64_t info_refresh_left_ms;

    uint8_t offered_addr[16];
    uint8_t action_addr[16];
    uint8_t has_offer;
    uint8_t has_action_addr;

    uint8_t tx_tries;
    uint8_t done;
} dhcpv6_bind_t;

#define DHCPV6_SOL_TIMEOUT_MS 1000
#define DHCPV6_SOL_MAX_RT_MS 3600000
#define DHCPV6_REQ_TIMEOUT_MS 1000
#define DHCPV6_REQ_MAX_RT_MS 30000
#define DHCPV6_REQ_MAX_RC 10
#define DHCPV6_CNF_TIMEOUT_MS 1000
#define DHCPV6_CNF_MAX_RT_MS 4000
#define DHCPV6_CNF_MAX_RD_MS 10000
#define DHCPV6_REN_TIMEOUT_MS 10000
#define DHCPV6_REN_MAX_RT_MS 600000
#define DHCPV6_REB_TIMEOUT_MS 10000
#define DHCPV6_REB_MAX_RT_MS 600000
#define DHCPV6_INF_TIMEOUT_MS 1000
#define DHCPV6_INF_MAX_RT_MS 3600000
#define DHCPV6_REL_TIMEOUT_MS 1000
#define DHCPV6_REL_MAX_RC 4
#define DHCPV6_DEC_TIMEOUT_MS 1000
#define DHCPV6_DEC_MAX_RC 4
#define DHCPV6_IRT_DEFAULT_SEC 86400
#define DHCPV6_IRT_MINIMUM_SEC 600
#define DHCPV6_ACTIVE_TICK_MS 100
#define DHCPV6_IDLE_TICK_MS 5000

static volatile uint8_t g_dhcpv6_running;
static volatile uint8_t g_dhcpv6_pending;
static volatile uint8_t g_dhcpv6_rekick;
static rng_t g_dhcpv6_rng;
static linked_list_t* g_dhcpv6_binds = NULL;

static volatile bool g_force_renew_all = false;
static volatile bool g_force_rebind_all = false;
static volatile bool g_force_confirm_all = false;

static uint32_t g_force_release[MAX_IPV6_L3_INTERFACES];
static uint32_t g_force_decline[MAX_IPV6_L3_INTERFACES];

void dhcpv6_force_renew_all() {
    irq_flags_t irq = irq_save_disable();
    g_force_renew_all = true;
    irq_restore(irq);
    dhcpv6_daemon_kick();
}
void dhcpv6_force_rebind_all() {
    irq_flags_t irq = irq_save_disable();
    g_force_rebind_all = true;
    irq_restore(irq);
    dhcpv6_daemon_kick();
}
void dhcpv6_force_confirm_all() {
    irq_flags_t irq = irq_save_disable();
    g_force_confirm_all = true;
    irq_restore(irq);
    dhcpv6_daemon_kick();
}

void dhcpv6_force_release_l3(l3_id_t l3_id) {
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    if (!v6 || v6->cfg != IPV6_CFG_DHCPV6) return;
    uint32_t index = (uint32_t)l3_id - MAX_IPV4_L3_INTERFACES - 1;
    if (index >= MAX_IPV6_L3_INTERFACES) return;
    irq_flags_t irq = irq_save_disable();
    g_force_release[index] = v6->generation;
    irq_restore(irq);
    dhcpv6_daemon_kick();
}

void dhcpv6_force_decline_l3(l3_id_t l3_id) {
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    if (!v6 || v6->cfg != IPV6_CFG_DHCPV6) return;
    uint32_t index = (uint32_t)l3_id - MAX_IPV4_L3_INTERFACES - 1;
    if (index >= MAX_IPV6_L3_INTERFACES) return;
    irq_flags_t irq = irq_save_disable();
    g_force_decline[index] = v6->generation;
    irq_restore(irq);
    dhcpv6_daemon_kick();
}

static void reset_backoff(dhcpv6_bind_t* b) {
    if (!b) return;
    b->backoff_ms = 0;
    b->retry_left_ms = 0;
    b->rx_fast_left_ms = 0;
    b->exchange_elapsed_ms = 0;
    b->tx_tries = 0;
}

static void start_exchange(l3_ipv6_interface_t* v6, dhcpv6_bind_t* b, uint8_t state) {
    if (!v6 || !b) return;
    v6->dhcpv6_state = state;
    b->xid24 = dhcpv6_make_xid24(rng_next32(&g_dhcpv6_rng));
    reset_backoff(b);

    if (state == DHCPV6_S_SOLICIT || state == DHCPV6_S_CONFIRMING || state == DHCPV6_S_INFO) b->retry_left_ms = rng_next32(&g_dhcpv6_rng) % 1000;
    if (state == DHCPV6_S_SOLICIT) {
        b->has_offer = 0;
        memset(b->offered_addr, 0, sizeof(b->offered_addr));
        v6->runtime_opts_v6.server_id_len = 0;
    }
}

static void reset_lease_state(l3_ipv6_interface_t* v6, dhcpv6_bind_t* b) {
    if (v6) {
        if (v6->cfg == IPV6_CFG_DHCPV6) {
            if (!ipv6_is_unspecified(v6->ip) || v6->prefix_len || !ipv6_is_unspecified(v6->gateway)) {
                l3_ipv6_update(v6->l3_id, (const uint8_t[16]){0}, 0, v6->gateway, IPV6_CFG_DHCPV6, v6->kind);
                if (b) b->target_generation = v6->generation;
            }
        }
        v6->dhcpv6_state = DHCPV6_S_INIT;
        v6->runtime_opts_v6.server_id_len = 0;
        v6->runtime_opts_v6.lease = 0;
        v6->runtime_opts_v6.lease_start_time = 0;
        v6->runtime_opts_v6.t1 = 0;
        v6->runtime_opts_v6.t2 = 0;
        memset(v6->runtime_opts_v6.pd_prefix, 0, sizeof(v6->runtime_opts_v6.pd_prefix));
        v6->runtime_opts_v6.pd_prefix_len = 0;
        v6->runtime_opts_v6.pd_preferred_lft = 0;
        v6->runtime_opts_v6.pd_valid_lft = 0;
    }
    if (b) {
        b->t1_left_ms = 0;
        b->t2_left_ms = 0;
        b->lease_left_ms = 0;
        b->xid24 = 0;
        reset_backoff(b);
        b->has_offer = 0;
        b->has_action_addr = 0;
    }
}

static void ensure_binds() {
    if (!g_dhcpv6_binds) g_dhcpv6_binds = linked_list_create();
    if (!g_dhcpv6_binds) return;

    linked_list_node_t* it = g_dhcpv6_binds->head;
    while (it) {
        linked_list_node_t* nxt = it->next;
        dhcpv6_bind_t* b = (dhcpv6_bind_t*)it->data;

        bool keep = true;

        if (!b) keep = false;

        l2_interface_t* l2 = NULL;
        if (keep) {
            l2 = l2_interface_find_by_index(b->ifindex);
            if (!l2 || !l2->is_up) keep = false;
        }

        l3_ipv6_interface_t* t = NULL;
        if (keep) {
            t = l3_ipv6_find_by_id(b->target_l3_id);
            if (!t || t->generation != b->target_generation) keep = false;
        }

        if (keep) {
            bool stateful = (t->cfg == IPV6_CFG_DHCPV6);
            bool stateless = ((t->cfg & IPV6_CFG_STATELESS) == IPV6_CFG_STATELESS && t->dhcpv6_stateless);
            if (!stateful && !stateless) keep = false;
        }
        if (keep && (!t->l2 || !t->l2->is_up)) keep = false;

        l3_ipv6_interface_t* llv6 = NULL;
        if (keep) {
            llv6 = l3_ipv6_find_by_id(b->bound_linklocal_l3_id);
            if (!llv6 || llv6->generation != b->bound_linklocal_generation || !ipv6_l3_is_ready(llv6) || !ipv6_is_linklocal(llv6->ip)) keep = false;
        }

        if (!keep) {
            if (t && t->generation == b->target_generation && t->cfg == IPV6_CFG_DHCPV6) reset_lease_state(t, b);

            dhcpv6_bind_t* rb = (dhcpv6_bind_t*)linked_list_remove(g_dhcpv6_binds, it);
            if (rb) {
                if (rb->sock) close_socket(rb->sock);
                free_sized(rb, sizeof(*rb));
            }
        }

        it = nxt;
    }

    uint8_t n = l2_interface_count();
    for (uint8_t ix = 0; ix < n; ix++) {
        l2_interface_t* l2 = l2_interface_at(ix);
        if (!l2 || !l2->is_up) continue;

        bool already = false;
        for (linked_list_node_t* it2 = g_dhcpv6_binds->head; it2; it2 = it2->next) {
            dhcpv6_bind_t* b = (dhcpv6_bind_t*)it2->data;
            if (b && b->ifindex == l2->ifindex) {
                already = true;
                break;
            }
        }
        if (already) continue;

        l3_ipv6_interface_t* target = NULL;
        for (int s = 0; s < MAX_IPV6_PER_INTERFACE; s++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!v6) continue;
            if (!(v6->kind & IPV6_ADDRK_GLOBAL)) continue;

            if (v6->cfg == IPV6_CFG_DHCPV6) {
                target = v6;
                break;
            }
            if (!target && (v6->cfg & IPV6_CFG_STATELESS) == IPV6_CFG_STATELESS && v6->dhcpv6_stateless) target = v6;
        }
        if (!target) continue;

        l3_id_t ll_l3 = 0;
        bool ll_ok = false;

        for (int i = 0; i < MAX_IPV6_PER_INTERFACE; i++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[i];
            if (!ipv6_l3_is_ready(v6)) continue;
            if (!ipv6_is_linklocal(v6->ip)) continue;
            ll_l3 = v6->l3_id;
            ll_ok = true;
            break;
        }
        if (!ll_ok) continue;

        dhcpv6_bind_t* b = (dhcpv6_bind_t*)zalloc(sizeof(*b));
        if (!b) continue;

        b->ifindex = l2->ifindex;
        b->target_l3_id = target->l3_id;
        b->bound_linklocal_l3_id = ll_l3;
        b->target_generation = target->generation;
        l3_ipv6_interface_t* llv6 = l3_ipv6_find_by_id(ll_l3);
        b->bound_linklocal_generation = llv6 ? llv6->generation : 0;
        b->last_tick_ms = (uint32_t)get_time();
        b->sol_max_rt_ms = DHCPV6_SOL_MAX_RT_MS;
        b->inf_max_rt_ms = DHCPV6_INF_MAX_RT_MS;

        const uint8_t* mac = network_get_mac(b->ifindex);
        if (mac) {
            mac_copy(b->mac, mac);
            b->mac_ok = 1;
        }

        b->sock = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_NONBLOCK});
        if (!b->sock) {
            free_sized(b, sizeof(*b));
            continue;
        }

        SockBindSpec spec;
        memset(&spec, 0, sizeof(spec));
        spec.kind = BIND_L3;
        spec.ver = IP_VER6;
        spec.l3_id = b->bound_linklocal_l3_id;

        if (bind_socket(b->sock, &spec, DHCPV6_CLIENT_PORT) != SOCK_OK) {
            close_socket(b->sock);
            free_sized(b, sizeof(*b));
            continue;
        }

        linked_list_push_front(g_dhcpv6_binds, b);
    }
}

static void fsm_once(dhcpv6_bind_t* b, uint32_t tick_ms, bool force_renew, bool force_rebind, bool force_confirm) {
    if (!b || !b->mac_ok || !b->sock) return;
    if (b->done) return;

    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(b->target_l3_id);
    if (!v6 || v6->generation != b->target_generation) return;
    l3_ipv6_interface_t* llv6 = l3_ipv6_find_by_id(b->bound_linklocal_l3_id);
    if (!llv6 || llv6->generation != b->bound_linklocal_generation) return;
    bool stateful = (v6->cfg == IPV6_CFG_DHCPV6);
    bool stateless = ((v6->cfg & IPV6_CFG_STATELESS) == IPV6_CFG_STATELESS && v6->dhcpv6_stateless);

    if (!stateful && !stateless) return;
    if (!(v6->kind & IPV6_ADDRK_GLOBAL)) return;
    if (stateless && v6->dhcpv6_stateless_done) {
        if (b->info_refresh_left_ms != UINT64_MAX) {
            if (b->info_refresh_left_ms > tick_ms) b->info_refresh_left_ms -= tick_ms;
            else b->info_refresh_left_ms = 0;
        }
        if (b->info_refresh_left_ms) return;
        v6->dhcpv6_stateless_done = 0;
    }

    if (!v6->runtime_opts_v6.iaid) v6->runtime_opts_v6.iaid = dhcpv6_iaid_from_mac(b->mac);
    if (!v6->runtime_opts_v6.iaid) v6->runtime_opts_v6.iaid = rng_next32(&g_dhcpv6_rng);

    if (v6->runtime_opts_v6.lease && v6->runtime_opts_v6.lease_start_time && !ipv6_is_unspecified(v6->ip) &&
        (v6->dhcpv6_state == DHCPV6_S_INIT || v6->dhcpv6_state == DHCPV6_S_BOUND || v6->dhcpv6_state == DHCPV6_S_RENEWING || v6->dhcpv6_state == DHCPV6_S_REBINDING || v6->dhcpv6_state == DHCPV6_S_CONFIRMING)) {
        uint64_t age_ms = get_time() - v6->runtime_opts_v6.lease_start_time;
        uint32_t lease_s = v6->runtime_opts_v6.lease;
        uint32_t t1_s = v6->runtime_opts_v6.t1;
        uint32_t t2_s = v6->runtime_opts_v6.t2;

        if (!t1_s) t1_s = lease_s == UINT32_MAX ? UINT32_MAX : lease_s / 2;
        if (!t2_s) t2_s = lease_s == UINT32_MAX ? UINT32_MAX : (uint64_t)lease_s * 4 / 5;

        uint64_t lease_ms = lease_s == UINT32_MAX ? UINT64_MAX : (uint64_t)lease_s * 1000;
        uint64_t t1_ms = t1_s == UINT32_MAX ? UINT64_MAX : (uint64_t)t1_s * 1000;
        uint64_t t2_ms = t2_s == UINT32_MAX ? UINT64_MAX : (uint64_t)t2_s * 1000;

        if (lease_ms != UINT64_MAX && age_ms >= lease_ms) reset_lease_state(v6, b);
        else {
            if (lease_ms == UINT64_MAX) b->lease_left_ms = UINT64_MAX;
            else b->lease_left_ms = lease_ms - age_ms;

            if (t1_ms == UINT64_MAX) b->t1_left_ms = UINT64_MAX;
            else if (age_ms >= t1_ms) b->t1_left_ms = 0;
            else b->t1_left_ms = t1_ms - age_ms;

            if (t2_ms == UINT64_MAX) b->t2_left_ms = UINT64_MAX;
            else if (age_ms >= t2_ms) b->t2_left_ms = 0;
            else b->t2_left_ms = t2_ms - age_ms;

            if (v6->dhcpv6_state == DHCPV6_S_INIT) v6->dhcpv6_state = DHCPV6_S_BOUND;
        }
    }

    uint8_t state = v6->dhcpv6_state;
    if (b->retry_left_ms > tick_ms) b->retry_left_ms -= tick_ms;
    else b->retry_left_ms = 0;
    if (b->rx_fast_left_ms > tick_ms) b->rx_fast_left_ms -= tick_ms;
    else b->rx_fast_left_ms = 0;
    if (state != DHCPV6_S_INIT && state != DHCPV6_S_BOUND) b->exchange_elapsed_ms += tick_ms;

    uint32_t force_index = (uint32_t)v6->l3_id - MAX_IPV4_L3_INTERFACES - 1;
    bool do_release = false;
    bool do_decline = false;
    if (force_index < MAX_IPV6_L3_INTERFACES) {
        irq_flags_t irq = irq_save_disable();
        do_release = g_force_release[force_index] == v6->generation;
        do_decline = g_force_decline[force_index] == v6->generation;
        g_force_release[force_index] = 0;
        g_force_decline[force_index] = 0;
        irq_restore(irq);
    }

    if (stateful && do_release) {
        if (v6->runtime_opts_v6.server_id_len && !ipv6_is_unspecified(v6->ip)) {
            ipv6_cpy(b->action_addr, v6->ip);
            b->has_action_addr = 1;
            l3_ipv6_update(v6->l3_id, (const uint8_t[16]){0}, 0, v6->gateway, IPV6_CFG_DHCPV6, v6->kind);
            b->target_generation = v6->generation;
            start_exchange(v6, b, DHCPV6_S_RELEASING);
        } else {
            reset_lease_state(v6, b);
            b->done = 1;
            return;
        }
    } else if (stateful && do_decline) {
        if (v6->runtime_opts_v6.server_id_len && !ipv6_is_unspecified(v6->ip)) {
            ipv6_cpy(b->action_addr, v6->ip);
            b->has_action_addr = 1;
            l3_ipv6_update(v6->l3_id, (const uint8_t[16]){0}, 0, v6->gateway, IPV6_CFG_DHCPV6, v6->kind);
            b->target_generation = v6->generation;
            start_exchange(v6, b, DHCPV6_S_DECLINING);
        } else {
            reset_lease_state(v6, b);
            start_exchange(v6, b, DHCPV6_S_SOLICIT);
        }
    } else if (stateful && force_confirm && !ipv6_is_unspecified(v6->ip)) start_exchange(v6, b, DHCPV6_S_CONFIRMING);
    else if (stateful && force_rebind && !ipv6_is_unspecified(v6->ip)) start_exchange(v6, b, DHCPV6_S_REBINDING);
    else if (stateful && force_renew && v6->runtime_opts_v6.server_id_len && !ipv6_is_unspecified(v6->ip)) start_exchange(v6, b, DHCPV6_S_RENEWING);

    if (stateful && !ipv6_is_unspecified(v6->ip) && !ipv6_is_unspecified(v6->gateway)) {
        if (!b->last_gateway_ok) {
            ipv6_cpy(b->last_gateway, v6->gateway);
            b->last_gateway_ok = 1;
        } else if (ipv6_cmp(b->last_gateway, v6->gateway) != 0) {
            ipv6_cpy(b->last_gateway, v6->gateway);
            start_exchange(v6, b, DHCPV6_S_CONFIRMING);
        }
    }

    if (stateful && v6->runtime_opts_v6.lease && !b->lease_left_ms) {
        reset_lease_state(v6, b);
        start_exchange(v6, b, DHCPV6_S_SOLICIT);
    }

    if (b->xid24) {
        for (int packets = 0; packets < 4; packets++) {
            uint8_t rx[DHCPV6_MAX_MSG];
            net_l4_endpoint src;
            memset(&src, 0, sizeof(src));
            int64_t r = receive_from_socket(b->sock, rx, sizeof(rx), &src);
            if (r <= 0) break;
            if (src.port != DHCPV6_SERVER_PORT || r < 4) continue;

            dhcpv6_parsed_t p;
            if (!dhcpv6_parse_message(rx, r, b->xid24, v6->runtime_opts_v6.iaid, &p)) continue;
            if (!p.has_server_id || !p.has_client_id || p.client_id_len != 10) continue;

            uint8_t duid[10];
            uint16_t duid_type = bswap16(3);
            uint16_t hw_type = bswap16(1);
            memcpy(duid, &duid_type, 2);
            memcpy(duid + 2, &hw_type, 2);
            mac_copy(duid + 4, b->mac);
            if (memcmp(duid, p.client_id, sizeof(duid)) != 0) continue;

            if (p.has_sol_max_rt) b->sol_max_rt_ms = p.sol_max_rt * 1000;
            if (p.has_inf_max_rt) b->inf_max_rt_ms = p.inf_max_rt * 1000;

            state = v6->dhcpv6_state;
            bool success = (!p.has_status || p.status_code == DHCPV6_STATUS_SUCCESS) && (!p.has_ia_status || p.ia_status_code == DHCPV6_STATUS_SUCCESS);
            bool no_binding = (p.has_status && p.status_code == DHCPV6_STATUS_NO_BINDING) || (p.has_ia_status && p.ia_status_code == DHCPV6_STATUS_NO_BINDING);
            bool not_on_link = (p.has_status && p.status_code == DHCPV6_STATUS_NOT_ON_LINK) || (p.has_ia_status && p.ia_status_code == DHCPV6_STATUS_NOT_ON_LINK);

            if (state == DHCPV6_S_SOLICIT) {
                if (p.msg_type != DHCPV6_MSG_ADVERTISE || !success || !p.has_ia_na || !p.has_addr || !p.valid_lft) continue;
                v6->runtime_opts_v6.server_id_len = p.server_id_len;
                memcpy(v6->runtime_opts_v6.server_id, p.server_id, p.server_id_len);
                if (p.has_pd) {
                    ipv6_cpy(v6->runtime_opts_v6.pd_prefix, p.pd_prefix);
                    v6->runtime_opts_v6.pd_prefix_len = p.pd_prefix_len;
                    v6->runtime_opts_v6.pd_preferred_lft = p.pd_preferred_lft;
                    v6->runtime_opts_v6.pd_valid_lft = p.pd_valid_lft;
                }
                ipv6_cpy(b->offered_addr, p.addr);
                b->has_offer = 1;
                start_exchange(v6, b, DHCPV6_S_REQUEST);
                return;
            }

            if (p.msg_type != DHCPV6_MSG_REPLY) continue;
            if (state == DHCPV6_S_REQUEST || state == DHCPV6_S_RENEWING || state == DHCPV6_S_RELEASING || state == DHCPV6_S_DECLINING) {

                if (!v6->runtime_opts_v6.server_id_len || p.server_id_len != v6->runtime_opts_v6.server_id_len || memcmp(p.server_id, v6->runtime_opts_v6.server_id, p.server_id_len) != 0) continue;
            }

            if (state == DHCPV6_S_INFO) {
                if (!stateless || !success) continue;
                v6->runtime_opts_v6.server_id_len = p.server_id_len;
                memcpy(v6->runtime_opts_v6.server_id, p.server_id, p.server_id_len);
                if (p.has_dns) memcpy(v6->runtime_opts_v6.dns, p.dns, sizeof(v6->runtime_opts_v6.dns));
                if (p.has_ntp) memcpy(v6->runtime_opts_v6.ntp, p.ntp, sizeof(v6->runtime_opts_v6.ntp));

                uint32_t refresh_s = p.has_info_refresh_time ? p.info_refresh_time : DHCPV6_IRT_DEFAULT_SEC;
                if (refresh_s != UINT32_MAX && refresh_s < DHCPV6_IRT_MINIMUM_SEC) refresh_s = DHCPV6_IRT_MINIMUM_SEC;
                if (refresh_s == UINT32_MAX) b->info_refresh_left_ms = UINT64_MAX;
                else b->info_refresh_left_ms = (uint64_t)refresh_s * 1000;
                v6->dhcpv6_stateless_done = 1;
                v6->dhcpv6_state = DHCPV6_S_INIT;
                b->xid24 = 0;
                reset_backoff(b);
                return;
            }

            if (state == DHCPV6_S_CONFIRMING) {
                if (!p.has_status) continue;
                if (p.status_code == DHCPV6_STATUS_SUCCESS) {
                    v6->dhcpv6_state = DHCPV6_S_BOUND;
                    b->xid24 = 0;
                    reset_backoff(b);
                    return;
                }
                if (p.status_code == DHCPV6_STATUS_NOT_ON_LINK) {
                    reset_lease_state(v6, b);
                    start_exchange(v6, b, DHCPV6_S_SOLICIT);
                    return;
                }
                continue;
            }

            if (state == DHCPV6_S_RELEASING) {
                if (!success && !no_binding) continue;
                reset_lease_state(v6, b);
                b->done = 1;
                return;
            }
            if (state == DHCPV6_S_DECLINING) {
                if (!success && !no_binding) continue;
                reset_lease_state(v6, b);
                start_exchange(v6, b, DHCPV6_S_SOLICIT);
                return;
            }
            if (state == DHCPV6_S_REQUEST && not_on_link) {
                reset_lease_state(v6, b);
                start_exchange(v6, b, DHCPV6_S_SOLICIT);
                return;
            }
            if (state == DHCPV6_S_REQUEST && p.has_expired_addr) {
                reset_lease_state(v6, b);
                start_exchange(v6, b, DHCPV6_S_SOLICIT);
                return;
            }
            if ((state == DHCPV6_S_RENEWING || state == DHCPV6_S_REBINDING) && p.has_expired_addr && !ipv6_is_unspecified(v6->ip) && ipv6_cmp(p.expired_addr, v6->ip) == 0) {
                reset_lease_state(v6, b);
                start_exchange(v6, b, DHCPV6_S_SOLICIT);
                return;
            }
            if ((state == DHCPV6_S_RENEWING || state == DHCPV6_S_REBINDING) && no_binding) {
                v6->runtime_opts_v6.server_id_len = p.server_id_len;
                memcpy(v6->runtime_opts_v6.server_id, p.server_id, p.server_id_len);
                if (!ipv6_is_unspecified(v6->ip)) {
                    ipv6_cpy(b->offered_addr, v6->ip);
                    b->has_offer = 1;
                }
                start_exchange(v6, b, DHCPV6_S_REQUEST);
                return;
            }
            if (!success) continue;
            if (state != DHCPV6_S_REQUEST && state != DHCPV6_S_RENEWING && state != DHCPV6_S_REBINDING) continue;
            if (!p.has_ia_na || !p.has_addr || !p.valid_lft) continue;

            uint32_t lease_s = p.valid_lft;
            uint32_t preferred_s = p.preferred_lft ? p.preferred_lft : lease_s;
            uint32_t t1_s = p.t1;
            uint32_t t2_s = p.t2;
            if (!t1_s) t1_s = preferred_s == UINT32_MAX ? UINT32_MAX : preferred_s / 2;
            if (!t2_s) t2_s = preferred_s == UINT32_MAX ? UINT32_MAX : (uint64_t)preferred_s * 4 / 5;
            if (t1_s && t2_s && t1_s > t2_s) continue;

            uint8_t gw[16];

            if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) ipv6_cpy(gw, v6->gateway);
            else memset(gw, 0, 16);

            if (!l3_ipv6_update(v6->l3_id, p.addr, 128, gw, IPV6_CFG_DHCPV6, v6->kind)) continue;
            b->target_generation = v6->generation;

            v6->runtime_opts_v6.server_id_len = p.server_id_len;
            memcpy(v6->runtime_opts_v6.server_id, p.server_id, p.server_id_len);

            if (p.has_dns) memcpy(v6->runtime_opts_v6.dns, p.dns, sizeof(v6->runtime_opts_v6.dns));
            if (p.has_ntp) memcpy(v6->runtime_opts_v6.ntp, p.ntp, sizeof(v6->runtime_opts_v6.ntp));
            if (p.has_pd) {
                ipv6_cpy(v6->runtime_opts_v6.pd_prefix, p.pd_prefix);
                v6->runtime_opts_v6.pd_prefix_len = p.pd_prefix_len;
                v6->runtime_opts_v6.pd_preferred_lft = p.pd_preferred_lft;
                v6->runtime_opts_v6.pd_valid_lft = p.pd_valid_lft;
            }

            v6->runtime_opts_v6.t1 = t1_s;
            v6->runtime_opts_v6.t2 = t2_s;
            v6->runtime_opts_v6.lease = lease_s;
            v6->runtime_opts_v6.lease_start_time = get_time();

            b->t1_left_ms = t1_s == UINT32_MAX ? UINT64_MAX : (uint64_t)t1_s * 1000;
            b->t2_left_ms = t2_s == UINT32_MAX ? UINT64_MAX : (uint64_t)t2_s * 1000;
            b->lease_left_ms = lease_s == UINT32_MAX ? UINT64_MAX : (uint64_t)lease_s * 1000;
            b->has_offer = 0;
            b->has_action_addr = 0;

            v6->dhcpv6_state = DHCPV6_S_BOUND;
            b->xid24 = 0;
            reset_backoff(b);
            return;
        }
    }

    if (b->done) return;
    if (v6->dhcpv6_state == DHCPV6_S_INIT) {
        if (stateless) start_exchange(v6, b, DHCPV6_S_INFO);
        else start_exchange(v6, b, DHCPV6_S_SOLICIT);
    }
    if (v6->dhcpv6_state == DHCPV6_S_BOUND) {
        if (!b->t2_left_ms && b->lease_left_ms) start_exchange(v6, b, DHCPV6_S_REBINDING);
        else if (!b->t1_left_ms && b->lease_left_ms && v6->runtime_opts_v6.server_id_len) start_exchange(v6, b, DHCPV6_S_RENEWING);
        else return;
    }
    if (v6->dhcpv6_state == DHCPV6_S_RENEWING && !b->t2_left_ms && b->lease_left_ms) start_exchange(v6, b, DHCPV6_S_REBINDING);

    state = v6->dhcpv6_state;
    uint8_t max_tries = 0;
    uint32_t max_duration = 0;
    if (state == DHCPV6_S_REQUEST) max_tries = DHCPV6_REQ_MAX_RC;
    else if (state == DHCPV6_S_CONFIRMING) max_duration = DHCPV6_CNF_MAX_RD_MS;
    else if (state == DHCPV6_S_RELEASING) max_tries = DHCPV6_REL_MAX_RC;
    else if (state == DHCPV6_S_DECLINING) max_tries = DHCPV6_DEC_MAX_RC;

    if ((!b->retry_left_ms && max_tries && b->tx_tries >= max_tries) || (max_duration && b->exchange_elapsed_ms >= max_duration)) {
        if (state == DHCPV6_S_REQUEST) {
            reset_lease_state(v6, b);
            start_exchange(v6, b, DHCPV6_S_SOLICIT);
        } else if (state == DHCPV6_S_CONFIRMING) {
            v6->dhcpv6_state = DHCPV6_S_BOUND;
            b->xid24 = 0;
            reset_backoff(b);
        } else if (state == DHCPV6_S_RELEASING) {
            reset_lease_state(v6, b);
            b->done = 1;
        } else if (state == DHCPV6_S_DECLINING) {
            reset_lease_state(v6, b);
            start_exchange(v6, b, DHCPV6_S_SOLICIT);
        }
        return;
    }
    if (b->retry_left_ms) return;

    uint8_t type = 0;
    if (state == DHCPV6_S_SOLICIT) type = DHCPV6_MSG_SOLICIT;
    else if (state == DHCPV6_S_REQUEST) type = DHCPV6_MSG_REQUEST;
    else if (state == DHCPV6_S_RENEWING) type = DHCPV6_MSG_RENEW;
    else if (state == DHCPV6_S_REBINDING) type = DHCPV6_MSG_REBIND;
    else if (state == DHCPV6_S_CONFIRMING) type = DHCPV6_MSG_CONFIRM;
    else if (state == DHCPV6_S_RELEASING) type = DHCPV6_MSG_RELEASE;
    else if (state == DHCPV6_S_DECLINING) type = DHCPV6_MSG_DECLINE;
    else if (state == DHCPV6_S_INFO) type = DHCPV6_MSG_INFORMATION_REQUEST;
    if (!type) return;

    const uint8_t* ia_addr = NULL;
    if (state == DHCPV6_S_REQUEST && b->has_offer) ia_addr = b->offered_addr;
    else if ((state == DHCPV6_S_RENEWING || state == DHCPV6_S_REBINDING || state == DHCPV6_S_CONFIRMING) && !ipv6_is_unspecified(v6->ip)) ia_addr = v6->ip;
    else if ((state == DHCPV6_S_RELEASING || state == DHCPV6_S_DECLINING) && b->has_action_addr) ia_addr = b->action_addr;

    uint64_t elapsed_cs = b->exchange_elapsed_ms / 10;
    uint16_t elapsed = elapsed_cs > UINT16_MAX ? UINT16_MAX : elapsed_cs;
    uint8_t msg[DHCPV6_MAX_MSG];
    uint32_t msg_len = 0;
    if (!dhcpv6_build_message(msg, sizeof(msg), &msg_len, &v6->runtime_opts_v6, b->mac, type, b->xid24, elapsed, ia_addr)) {
        b->retry_left_ms = 1000;
        return;
    }

    net_l4_endpoint dst;
    memset(&dst, 0, sizeof(dst));
    dst.ver = IP_VER6;
    ipv6_make_multicast(2, IPV6_MCAST_DHCPV6_SERVERS, NULL, dst.ip);
    dst.port = DHCPV6_SERVER_PORT;
    if (send_to_socket(b->sock, &dst, msg, msg_len) >= 0) b->rx_fast_left_ms = 1000;
    b->tx_tries++;

    uint32_t initial_rt = 0;
    uint32_t max_rt = 0;
    if (state == DHCPV6_S_SOLICIT) {
        initial_rt = DHCPV6_SOL_TIMEOUT_MS;
        max_rt = b->sol_max_rt_ms ? b->sol_max_rt_ms : DHCPV6_SOL_MAX_RT_MS;
    } else if (state == DHCPV6_S_REQUEST) {
        initial_rt = DHCPV6_REQ_TIMEOUT_MS;
        max_rt = DHCPV6_REQ_MAX_RT_MS;
    } else if (state == DHCPV6_S_CONFIRMING) {
        initial_rt = DHCPV6_CNF_TIMEOUT_MS;
        max_rt = DHCPV6_CNF_MAX_RT_MS;
    } else if (state == DHCPV6_S_RENEWING) {
        initial_rt = DHCPV6_REN_TIMEOUT_MS;
        max_rt = DHCPV6_REN_MAX_RT_MS;
    } else if (state == DHCPV6_S_REBINDING) {
        initial_rt = DHCPV6_REB_TIMEOUT_MS;
        max_rt = DHCPV6_REB_MAX_RT_MS;
    } else if (state == DHCPV6_S_INFO) {
        initial_rt = DHCPV6_INF_TIMEOUT_MS;
        max_rt = b->inf_max_rt_ms ? b->inf_max_rt_ms : DHCPV6_INF_MAX_RT_MS;
    } else if (state == DHCPV6_S_RELEASING) initial_rt = DHCPV6_REL_TIMEOUT_MS;
    else if (state == DHCPV6_S_DECLINING) initial_rt = DHCPV6_DEC_TIMEOUT_MS;

    uint32_t base = initial_rt;
    if (b->backoff_ms) base = b->backoff_ms > UINT32_MAX / 2 ? UINT32_MAX : b->backoff_ms * 2;

    int32_t jitter = rng_next32(&g_dhcpv6_rng) % 20001;
    jitter -= 10000;
    int64_t next = base;
    next += next * jitter / 100000;
    if (next < 1) next = 1;

    if (max_rt && next > max_rt) {
        jitter = rng_next32(&g_dhcpv6_rng) % 20001;
        jitter -= 10000;
        next = max_rt;
        next += next * jitter / 100000;
        if (next < 1) next = 1;
    }
    if (next > UINT32_MAX) next = UINT32_MAX;
    b->backoff_ms = next;
    b->retry_left_ms = b->backoff_ms;
}

int dhcpv6_daemon_entry(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    irq_flags_t irq = irq_save_disable();
    g_dhcpv6_pending = 0;
    g_dhcpv6_running = 1;
    g_dhcpv6_rekick = 0;
    irq_restore(irq);

    rng_init_random(&g_dhcpv6_rng);

    for (;;) {
        irq = irq_save_disable();
        bool force_renew = g_force_renew_all;
        bool force_rebind = g_force_rebind_all;
        bool force_confirm = g_force_confirm_all;
        g_force_renew_all = false;
        g_force_rebind_all = false;
        g_force_confirm_all = false;
        g_dhcpv6_rekick = 0;
        irq_restore(irq);

        ensure_binds();

        if (g_dhcpv6_binds) {
            for (linked_list_node_t* it = g_dhcpv6_binds->head; it; it = it->next) {
                dhcpv6_bind_t* b = (dhcpv6_bind_t*)it->data;
                if (!b) continue;
                uint32_t now_ms = (uint32_t)get_time();
                uint32_t elapsed_ms = now_ms - b->last_tick_ms;
                b->last_tick_ms = now_ms;
                fsm_once(b, elapsed_ms, force_renew, force_rebind, force_confirm);
            }
        }


        bool active_work = g_force_renew_all || g_force_rebind_all || g_force_confirm_all;
        for (uint32_t i = 0; i < MAX_IPV6_L3_INTERFACES && !active_work; i++) active_work = g_force_release[i] || g_force_decline[i];
        if (!active_work && g_dhcpv6_binds) {
            for (linked_list_node_t* it = g_dhcpv6_binds->head; it; it = it->next) {
                dhcpv6_bind_t* b = (dhcpv6_bind_t*)it->data;
                if (!b || b->done || !b->sock) continue;
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(b->target_l3_id);
                if (!v6 || !v6->l2 || !v6->l2->is_up) continue;

                bool stateful = v6->cfg == IPV6_CFG_DHCPV6;
                bool stateless = (v6->cfg & IPV6_CFG_STATELESS) == IPV6_CFG_STATELESS && v6->dhcpv6_stateless;
                if (stateful || (stateless && (!v6->dhcpv6_stateless_done || b->info_refresh_left_ms != UINT64_MAX))) {
                    active_work = true;
                    break;
                }
            }
        }
        if (!active_work) break;

        uint32_t sleep_ms = g_dhcpv6_rekick ? DHCPV6_ACTIVE_TICK_MS : DHCPV6_IDLE_TICK_MS;
        if (g_dhcpv6_binds) {
            for (linked_list_node_t* it = g_dhcpv6_binds->head; it; it = it->next) {
                dhcpv6_bind_t* b = (dhcpv6_bind_t*)it->data;
                if (!b || b->done || !b->sock) continue;
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(b->target_l3_id);
                if (!v6 || !v6->l2 || !v6->l2->is_up) continue;

                uint32_t wait_ms = DHCPV6_IDLE_TICK_MS;
                uint8_t state = v6->dhcpv6_state;
                if (state != DHCPV6_S_INIT && state != DHCPV6_S_BOUND) {
                    if (b->rx_fast_left_ms) wait_ms = b->rx_fast_left_ms < DHCPV6_ACTIVE_TICK_MS ? b->rx_fast_left_ms : DHCPV6_ACTIVE_TICK_MS;
                    else if (b->retry_left_ms < wait_ms) wait_ms = b->retry_left_ms;
                    if (!b->retry_left_ms) wait_ms = DHCPV6_ACTIVE_TICK_MS;
                    if (state == DHCPV6_S_RENEWING && b->t2_left_ms < wait_ms) wait_ms = b->t2_left_ms;
                    if (b->lease_left_ms < wait_ms) wait_ms = b->lease_left_ms;
                } else if (state == DHCPV6_S_BOUND) {
                    uint64_t next = b->lease_left_ms;
                    if (b->t1_left_ms < next) next = b->t1_left_ms;
                    if (b->t2_left_ms < next) next = b->t2_left_ms;
                    if (next != UINT64_MAX && next < wait_ms) wait_ms = next;
                } else wait_ms = DHCPV6_ACTIVE_TICK_MS;

                bool stateless = (v6->cfg & IPV6_CFG_STATELESS) == IPV6_CFG_STATELESS && v6->dhcpv6_stateless;   
                if (stateless && v6->dhcpv6_stateless_done && b->info_refresh_left_ms != UINT64_MAX && b->info_refresh_left_ms < wait_ms) wait_ms = b->info_refresh_left_ms;
                if (!wait_ms) wait_ms = DHCPV6_ACTIVE_TICK_MS;
                if (wait_ms < sleep_ms) sleep_ms = wait_ms;
            }
        }
        msleep(sleep_ms);
    }

    if (g_dhcpv6_binds) {
        while (g_dhcpv6_binds->head) {
            dhcpv6_bind_t* b = (dhcpv6_bind_t*)linked_list_pop_front(g_dhcpv6_binds);
            if (!b) continue;
            if (b->sock) close_socket(b->sock);
            free_sized(b, sizeof(*b));
        }
        linked_list_destroy(g_dhcpv6_binds);
        g_dhcpv6_binds = NULL;
    }

    irq = irq_save_disable();
    g_dhcpv6_running = 0;
    bool rekick = g_dhcpv6_rekick != 0;
    g_dhcpv6_rekick = 0;
    irq_restore(irq);
    if (rekick) dhcpv6_daemon_kick();
    return 0;
}

void dhcpv6_daemon_kick(void) {
    irq_flags_t irq = irq_save_disable();
    if (g_dhcpv6_running || g_dhcpv6_pending) {
        g_dhcpv6_rekick = 1;
        irq_restore(irq);
        return;
    }
    irq_restore(irq);

    bool config_work = false;
    uint8_t n = l2_interface_count();
    for (uint8_t i = 0; i < n && !config_work; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;

        bool has_linklocal = false;
        for (int s = 0; s < MAX_IPV6_PER_INTERFACE; s++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (ipv6_l3_is_ready(v6) && ipv6_is_linklocal(v6->ip)) {
                has_linklocal = true;
                break;
            }
        }
        if (!has_linklocal) continue;

        for (int s = 0; s < MAX_IPV6_PER_INTERFACE; s++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[s];
            if (!v6 || !(v6->kind & IPV6_ADDRK_GLOBAL)) continue;
            if (v6->cfg == IPV6_CFG_DHCPV6 || ((v6->cfg & IPV6_CFG_STATELESS) == IPV6_CFG_STATELESS && v6->dhcpv6_stateless && !v6->dhcpv6_stateless_done)) {
                config_work = true;
                break;
            }
        }
    }
    bool forced_work = g_force_renew_all || g_force_rebind_all || g_force_confirm_all;
    for (uint32_t i = 0; i < MAX_IPV6_L3_INTERFACES && !forced_work; i++) forced_work = g_force_release[i] || g_force_decline[i];
    if (!config_work && !forced_work) return;

    irq = irq_save_disable();
    if (g_dhcpv6_running || g_dhcpv6_pending) {
        g_dhcpv6_rekick = 1;
        irq_restore(irq);
        return;
    }
    g_dhcpv6_pending = 1;
    irq_restore(irq);

    if (!create_kernel_process("dhcpv6_daemon", dhcpv6_daemon_entry, 0, 0)) {
        irq = irq_save_disable();
        g_dhcpv6_pending = 0;
        irq_restore(irq);
    }
}