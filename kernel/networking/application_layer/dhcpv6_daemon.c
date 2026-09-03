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
//TODO this code is a mess. cleanup
enum {
    DHCPV6_S_INIT = 0,
    DHCPV6_S_SOLICIT = 1,
    DHCPV6_S_REQUEST = 2,
    DHCPV6_S_BOUND = 3,
    DHCPV6_S_RENEWING = 4,
    DHCPV6_S_REBINDING = 5,
    DHCPV6_S_CONFIRMING = 6,
    DHCPV6_S_RELEASING = 7,
    DHCPV6_S_DECLINING = 8
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

    uint32_t t1_left_ms;
    uint32_t t2_left_ms;
    uint32_t lease_left_ms;
    uint32_t last_tick_ms;
    uint8_t last_state;
    uint8_t tx_tries;
    uint8_t done;
} dhcpv6_bind_t;

#define DHCPV6_MAX_INFOREQ_TX 3
#define DHCPV6_MAX_REQUEST_TX 3
#define DHCPV6_MAX_OTHER_TX 5

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
    g_force_renew_all = true;
    dhcpv6_daemon_kick();
}
void dhcpv6_force_rebind_all() {
    g_force_rebind_all = true;
    dhcpv6_daemon_kick();
}
void dhcpv6_force_confirm_all() {
    g_force_confirm_all = true;
    dhcpv6_daemon_kick();
}

void dhcpv6_force_release_l3(l3_id_t l3_id) {
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    if (!v6 || v6->cfg != IPV6_CFG_DHCPV6) return;
    uint32_t index = (uint32_t)l3_id - MAX_IPV4_L3_INTERFACES - 1;
    if (index >= MAX_IPV6_L3_INTERFACES) return;
    g_force_release[index] = v6->generation;
    dhcpv6_daemon_kick();
}

void dhcpv6_force_decline_l3(l3_id_t l3_id) {
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    if (!v6 || v6->cfg != IPV6_CFG_DHCPV6) return;
    uint32_t index = (uint32_t)l3_id - MAX_IPV4_L3_INTERFACES - 1;
    if (index >= MAX_IPV6_L3_INTERFACES) return;
    g_force_decline[index] = v6->generation;
    dhcpv6_daemon_kick();
}

static uint32_t next_backoff_ms(dhcpv6_bind_t* b) {
    if (!b) return 4000;

    if (!b->backoff_ms) b->backoff_ms = 4000;
    else {
        uint64_t d = (uint64_t)b->backoff_ms * 2u;
        if (d > 64000u) d = 64000u;
        b->backoff_ms = (uint32_t)d;
    }

    uint32_t j = (uint32_t)(rng_next32(&g_dhcpv6_rng) % 2000u);
    int64_t v = (int64_t)b->backoff_ms + (int64_t)((int32_t)j - 1000);
    if (v < 1000) v = 1000;

    return (uint32_t)v;
}

static void reset_backoff(dhcpv6_bind_t* b) {
    if (!b) return;
    b->backoff_ms = 0;
    b->retry_left_ms = 0;
}

static void reset_lease_state(l3_ipv6_interface_t* v6, dhcpv6_bind_t* b) {
    if (v6) {
        if (v6->cfg == IPV6_CFG_DHCPV6) {
            if (!ipv6_is_unspecified(v6->ip) || v6->prefix_len || !ipv6_is_unspecified(v6->gateway)) {
                l3_ipv6_update(v6->l3_id, (const uint8_t[16]){0}, 0, (const uint8_t[16]){0}, IPV6_CFG_DHCPV6, v6->kind);
                if (b) b->target_generation = v6->generation;
            }
        }
        v6->dhcpv6_state = DHCPV6_S_INIT;
        v6->runtime_opts_v6.server_id_len = 0;
        v6->runtime_opts_v6.lease = 0;
        v6->runtime_opts_v6.lease_start_time = 0;
    }
    if (b) {
        b->t1_left_ms = 0;
        b->t2_left_ms = 0;
        b->lease_left_ms = 0;
        b->xid24 = 0;
        b->last_state = 0xFF;
        b->tx_tries = 0;
        reset_backoff(b);
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
            if (!llv6 || llv6->generation != b->bound_linklocal_generation) keep = false;
        }

        if (keep && !ipv6_l3_is_ready(llv6)) keep = false;
        if (keep && !ipv6_is_linklocal(llv6->ip)) keep = false;

        if (!keep) {
            if (t && t->generation == b->target_generation) reset_lease_state(t, b);

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
            if ((v6->cfg & IPV6_CFG_STATELESS) == IPV6_CFG_STATELESS && v6->dhcpv6_stateless) {
                target = v6;
                break;
            }
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
            b->sock = 0;
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
    if (stateless && v6->dhcpv6_stateless_done) return;

    if (v6->runtime_opts_v6.lease != 0 && v6->runtime_opts_v6.lease_start_time != 0 && !ipv6_is_unspecified(v6->ip) && v6->dhcpv6_state == DHCPV6_S_INIT) {
        uint32_t now_ms = get_time();
        uint32_t start_ms = v6->runtime_opts_v6.lease_start_time;
        uint32_t elapsed_s = (now_ms - start_ms) / 1000;

        uint32_t lease_s = v6->runtime_opts_v6.lease;
        if (elapsed_s >= lease_s) {
            reset_lease_state(v6, b);
        } else {
            uint32_t left_s = lease_s - elapsed_s;
            b->lease_left_ms = left_s * 1000u;

            uint32_t t1_s = v6->runtime_opts_v6.t1;
            uint32_t t2_s = v6->runtime_opts_v6.t2;

            if (!t1_s) t1_s = lease_s / 2;
            if (!t2_s) t2_s = (lease_s / 8) * 7;

            if (elapsed_s >= t1_s) b->t1_left_ms = 0;
            else b->t1_left_ms = (t1_s - elapsed_s) * 1000u;

            if (elapsed_s >= t2_s) b->t2_left_ms = 0;
            else b->t2_left_ms = (t2_s - elapsed_s) * 1000u;

            v6->dhcpv6_state = DHCPV6_S_BOUND;
            reset_backoff(b);
        }
    }

    if (b->retry_left_ms > tick_ms) b->retry_left_ms -= tick_ms;
    else b->retry_left_ms = 0;

    if ((v6->dhcpv6_state == DHCPV6_S_BOUND || v6->dhcpv6_state == DHCPV6_S_RENEWING || v6->dhcpv6_state == DHCPV6_S_REBINDING) && v6->runtime_opts_v6.lease && v6->runtime_opts_v6.lease_start_time) {
        uint32_t now_ms = (uint32_t)get_time();
        uint32_t age_ms = now_ms - v6->runtime_opts_v6.lease_start_time;
        uint32_t lease_ms = v6->runtime_opts_v6.lease * 1000;
        uint32_t t1_s = v6->runtime_opts_v6.t1 ? v6->runtime_opts_v6.t1 : v6->runtime_opts_v6.lease / 2;
        uint32_t t2_s = v6->runtime_opts_v6.t2 ? v6->runtime_opts_v6.t2 : (v6->runtime_opts_v6.lease / 8) * 7;
        uint32_t t1_ms = t1_s * 1000;
        uint32_t t2_ms = t2_s * 1000;
        b->lease_left_ms = age_ms < lease_ms ? lease_ms - age_ms : 0;
        b->t1_left_ms = age_ms < t1_ms ? t1_ms - age_ms : 0;
        b->t2_left_ms = age_ms < t2_ms ? t2_ms - age_ms : 0;
    }

    if (!v6->runtime_opts_v6.iaid) v6->runtime_opts_v6.iaid = dhcpv6_iaid_from_mac(b->mac);
    if (!v6->runtime_opts_v6.iaid) v6->runtime_opts_v6.iaid = rng_next32(&g_dhcpv6_rng);

    uint32_t force_index = (uint32_t)v6->l3_id - MAX_IPV4_L3_INTERFACES - 1;
    bool do_release = g_force_release[force_index] == v6->generation;
    bool do_decline = g_force_decline[force_index] == v6->generation;
    if (g_force_release[force_index]) g_force_release[force_index] = 0;
    if (g_force_decline[force_index]) g_force_decline[force_index] = 0;

    if (do_release) {
        v6->dhcpv6_state = DHCPV6_S_RELEASING;
        b->retry_left_ms = 0;
        reset_backoff(b);
    } else if (do_decline) {
        v6->dhcpv6_state = DHCPV6_S_DECLINING;
        b->retry_left_ms = 0;
        reset_backoff(b);
    }

    if (force_confirm) {
        v6->dhcpv6_state = DHCPV6_S_CONFIRMING;
        b->retry_left_ms = 0;
        reset_backoff(b);
    } else if (force_rebind) {
        v6->dhcpv6_state = DHCPV6_S_REBINDING;
        b->retry_left_ms = 0;
        reset_backoff(b);
    } else if (force_renew) {
        v6->dhcpv6_state = DHCPV6_S_RENEWING;
        b->retry_left_ms = 0;
        reset_backoff(b);
    }

    if (v6->gateway[0] || v6->gateway[1]) {
        if (!b->last_gateway_ok) {
            ipv6_cpy(b->last_gateway, v6->gateway);
            b->last_gateway_ok = 1;
        } else if (ipv6_cmp(b->last_gateway, v6->gateway) != 0) {
            ipv6_cpy(b->last_gateway, v6->gateway);
            v6->dhcpv6_state = DHCPV6_S_CONFIRMING;
            b->retry_left_ms = 0;
            reset_backoff(b);
        }
    }

    if (v6->dhcpv6_state == DHCPV6_S_INIT) {
        if (stateless) {
            int has_dns = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]) || !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);

            if (has_dns) {
                v6->dhcpv6_stateless_done = 1;
                b->retry_left_ms = 0;
                reset_backoff(b);
                return;
            }

            v6->dhcpv6_state = DHCPV6_S_SOLICIT;
            b->retry_left_ms = 0;
            reset_backoff(b);
            return;
        }

        v6->dhcpv6_state = DHCPV6_S_SOLICIT;
        b->retry_left_ms = 0;
        reset_backoff(b);
        return;
    }

    if (v6->dhcpv6_state == DHCPV6_S_BOUND) {
        if (!b->lease_left_ms && v6->runtime_opts_v6.lease) {
            reset_lease_state(v6, b);
            return;
        }

        if (!b->t2_left_ms && b->lease_left_ms) {
            v6->dhcpv6_state = DHCPV6_S_REBINDING;
            b->retry_left_ms = 0;
            reset_backoff(b);
            return;
        }

        if (!b->t1_left_ms && b->lease_left_ms) {
            v6->dhcpv6_state = DHCPV6_S_RENEWING;
            b->retry_left_ms = 0;
            reset_backoff(b);
            return;
        }

        return;
    }

    if ((v6->dhcpv6_state == DHCPV6_S_RENEWING || v6->dhcpv6_state == DHCPV6_S_REBINDING) && !b->lease_left_ms) {
        reset_lease_state(v6, b);
        return;
    }
    if (v6->dhcpv6_state == DHCPV6_S_RENEWING && !b->t2_left_ms && b->lease_left_ms) {
        v6->dhcpv6_state = DHCPV6_S_REBINDING;
        b->retry_left_ms = 0;
        reset_backoff(b);
    }

    if (b->retry_left_ms) return;
    if (b->last_state != (uint8_t)v6->dhcpv6_state) {
        b->last_state = (uint8_t)v6->dhcpv6_state;
        b->tx_tries = 0;
    }

    uint8_t type_peek = DHCPV6_MSG_SOLICIT;

    if (stateless) type_peek = DHCPV6_MSG_INFORMATION_REQUEST;
    else if (v6->dhcpv6_state == DHCPV6_S_SOLICIT) type_peek = DHCPV6_MSG_SOLICIT;
    else if (v6->dhcpv6_state == DHCPV6_S_REQUEST) type_peek = DHCPV6_MSG_REQUEST;
    else if (v6->dhcpv6_state == DHCPV6_S_RENEWING) type_peek = DHCPV6_MSG_RENEW;
    else if (v6->dhcpv6_state == DHCPV6_S_REBINDING) type_peek = DHCPV6_MSG_REBIND;
    else if (v6->dhcpv6_state == DHCPV6_S_CONFIRMING) type_peek = DHCPV6_MSG_CONFIRM;
    else if (v6->dhcpv6_state == DHCPV6_S_RELEASING) type_peek = DHCPV6_MSG_RELEASE;
    else if (v6->dhcpv6_state == DHCPV6_S_DECLINING) type_peek = DHCPV6_MSG_DECLINE;

    uint8_t lim = DHCPV6_MAX_OTHER_TX;
    if (type_peek == DHCPV6_MSG_INFORMATION_REQUEST) lim = DHCPV6_MAX_INFOREQ_TX;
    else if (type_peek == DHCPV6_MSG_REQUEST) lim = DHCPV6_MAX_REQUEST_TX;

    if (b->tx_tries >= lim) {
        int has_dns = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]) || !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);

        if (!has_dns) {
            if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) {
                ipv6_cpy(v6->runtime_opts_v6.dns[0], v6->gateway);
            }
        }

        if (stateless){
            v6->dhcpv6_stateless_done = 1;
            v6->dhcpv6_state = DHCPV6_S_INIT;
            b->retry_left_ms = 0;
            reset_backoff(b);
            return;
        }

        if (type_peek == DHCPV6_MSG_RELEASE || type_peek == DHCPV6_MSG_DECLINE) {
            reset_lease_state(v6, b);
            b->done = 1;
            return;
        }
        if (type_peek == DHCPV6_MSG_RENEW || type_peek == DHCPV6_MSG_REBIND) {
            b->tx_tries = 0;
            b->retry_left_ms = next_backoff_ms(b);
            b->last_tick_ms = (uint32_t)get_time();
            return;
        }

        b->done = 1;
        v6->dhcpv6_state = DHCPV6_S_INIT;
        reset_backoff(b);
        return;
    }
    uint8_t msg[DHCPV6_MAX_MSG];
    uint32_t msg_len = 0;

    b->xid24 = dhcpv6_make_xid24(rng_next32(&g_dhcpv6_rng));

    uint8_t type = DHCPV6_MSG_SOLICIT;
    dhcpv6_req_kind kind = DHCPV6K_SELECT;

    if (stateless) {
        type = DHCPV6_MSG_INFORMATION_REQUEST;
        kind = DHCPV6K_SELECT;
    } else if (v6->dhcpv6_state == DHCPV6_S_SOLICIT) {
        type = DHCPV6_MSG_SOLICIT;
        kind = DHCPV6K_SELECT;
    } else if (v6->dhcpv6_state == DHCPV6_S_REQUEST) {
        type = DHCPV6_MSG_REQUEST;
        kind = DHCPV6K_SELECT;
    } else if (v6->dhcpv6_state == DHCPV6_S_RENEWING) {
        type = DHCPV6_MSG_RENEW;
        kind = DHCPV6K_RENEW;
    } else if (v6->dhcpv6_state == DHCPV6_S_REBINDING) {
        type = DHCPV6_MSG_REBIND;
        kind = DHCPV6K_REBIND;
    } else if (v6->dhcpv6_state == DHCPV6_S_CONFIRMING) {
        type = DHCPV6_MSG_CONFIRM;
        kind = DHCPV6K_CONFIRM;
    } else if (v6->dhcpv6_state == DHCPV6_S_RELEASING) {
        type = DHCPV6_MSG_RELEASE;
        kind = DHCPV6K_RELEASE;
    } else if (v6->dhcpv6_state == DHCPV6_S_DECLINING) {
        type = DHCPV6_MSG_DECLINE;
        kind = DHCPV6K_DECLINE;
    } else {
        type = DHCPV6_MSG_SOLICIT;
        kind = DHCPV6K_SELECT;
        v6->dhcpv6_state = DHCPV6_S_SOLICIT;
    }
    bool want_addr = !stateless;

    if (!dhcpv6_build_message(msg, sizeof(msg), &msg_len, &v6->runtime_opts_v6, b->mac, type, kind, b->xid24, want_addr)) {
        b->retry_left_ms = next_backoff_ms(b);
        b->last_tick_ms = (uint32_t)get_time();
        return;
    }

    net_l4_endpoint dst;
    memset(&dst, 0, sizeof(dst));
    dst.ver = IP_VER6;
    ipv6_make_multicast(2, IPV6_MCAST_DHCPV6_SERVERS, NULL, dst.ip);
    dst.port = DHCPV6_SERVER_PORT;

    (void)send_to_socket(b->sock, &dst, msg, (uint64_t)msg_len);
    b->tx_tries++;

    uint8_t rx[DHCPV6_MAX_MSG];
    uint32_t rx_len = 0;

    net_l4_endpoint src;
    memset(&src, 0, sizeof(src));

    bool got = false;
    uint32_t waited = 0;

    while (waited < 250) {
        int64_t r = receive_from_socket(b->sock, rx, sizeof(rx), &src);
        if (r > 0) {
            if (src.port != DHCPV6_SERVER_PORT) {
                msleep(50);
                waited += 50;
                continue;
            }
            rx_len = (uint32_t)r;
            got = true;
            break;
        }
        msleep(50);
        waited += 50;
    }

    if (got && rx_len >= 4) {
        uint32_t rid24 = ((uint32_t)rx[1] << 16) | ((uint32_t)rx[2] << 8) | (uint32_t)rx[3];
        uint32_t expect = dhcpv6_make_xid24(b->xid24);

        if (rid24 == (expect & 0x00FFFFFFu)) {
            dhcpv6_parsed_t p;

            if (dhcpv6_parse_message(rx, rx_len, expect, v6->runtime_opts_v6.iaid, &p)) {
                if (p.msg_type == DHCPV6_MSG_ADVERTISE && v6->dhcpv6_state == DHCPV6_S_SOLICIT) {
                    if (p.has_server_id) {
                        v6->runtime_opts_v6.server_id_len = p.server_id_len;
                        if (p.server_id_len) memcpy(v6->runtime_opts_v6.server_id, p.server_id, p.server_id_len);
                    }

                    if (p.has_dns) memcpy(v6->runtime_opts_v6.dns, p.dns, sizeof(v6->runtime_opts_v6.dns));
                    if (p.has_ntp) memcpy(v6->runtime_opts_v6.ntp, p.ntp, sizeof(v6->runtime_opts_v6.ntp));
                    int has_dns = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]) || !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);

                    if (!has_dns) {
                        if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) {
                            ipv6_cpy(v6->runtime_opts_v6.dns[0], v6->gateway);
                        }
                    }

                    if (p.has_pd) {
                        ipv6_cpy(v6->runtime_opts_v6.pd_prefix, p.pd_prefix);
                        v6->runtime_opts_v6.pd_prefix_len = p.pd_prefix_len;
                        v6->runtime_opts_v6.pd_preferred_lft = p.pd_preferred_lft;
                        v6->runtime_opts_v6.pd_valid_lft = p.pd_valid_lft;
                    }

                    if (p.t1) v6->runtime_opts_v6.t1 = p.t1;
                    if (p.t2) v6->runtime_opts_v6.t2 = p.t2;

                    if (p.has_addr) {
                        uint8_t gw[16];

                        if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) ipv6_cpy(gw, v6->gateway);
                        else memset(gw, 0, 16);

                        (void)l3_ipv6_update(v6->l3_id, p.addr, 128, gw, IPV6_CFG_DHCPV6, v6->kind);
                        b->target_generation = v6->generation;

                        uint32_t lease_s = p.valid_lft;
                        v6->runtime_opts_v6.lease = lease_s;
                        v6->runtime_opts_v6.lease_start_time = get_time();

                        uint32_t t1_s = v6->runtime_opts_v6.t1;
                        uint32_t t2_s = v6->runtime_opts_v6.t2;

                        if (!t1_s) t1_s = lease_s / 2;
                        if (!t2_s) t2_s = (lease_s / 8) * 7;

                        b->t1_left_ms = t1_s * 1000u;
                        b->t2_left_ms = t2_s * 1000u;
                        b->lease_left_ms = lease_s * 1000u;
                    }

                    v6->dhcpv6_state = DHCPV6_S_REQUEST;
                    b->retry_left_ms = 0;
                    reset_backoff(b);
                } else if (p.msg_type == DHCPV6_MSG_REPLY) {
                    if (stateless) {
                        if (p.has_dns) memcpy(v6->runtime_opts_v6.dns, p.dns, sizeof(v6->runtime_opts_v6.dns));
                        if (p.has_ntp) memcpy(v6->runtime_opts_v6.ntp, p.ntp, sizeof(v6->runtime_opts_v6.ntp));

                        int has_dns = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]) || !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);

                        if (!has_dns) {
                            if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) {
                                ipv6_cpy(v6->runtime_opts_v6.dns[0], v6->gateway);
                            }
                        }

                        v6->dhcpv6_stateless_done = 1;
                        reset_backoff(b);
                        return;
                    }
                    if (v6->dhcpv6_state == DHCPV6_S_REQUEST || v6->dhcpv6_state == DHCPV6_S_RENEWING || v6->dhcpv6_state == DHCPV6_S_REBINDING || v6->dhcpv6_state == DHCPV6_S_CONFIRMING) {
                        if (p.has_server_id) {
                            v6->runtime_opts_v6.server_id_len = p.server_id_len;
                            if (p.server_id_len) memcpy(v6->runtime_opts_v6.server_id, p.server_id, p.server_id_len);
                        }

                        if (p.has_dns) memcpy(v6->runtime_opts_v6.dns, p.dns, sizeof(v6->runtime_opts_v6.dns));
                        if (p.has_ntp) memcpy(v6->runtime_opts_v6.ntp, p.ntp, sizeof(v6->runtime_opts_v6.ntp));
                        int has_dns = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]) || !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);

                        if (!has_dns) {
                            if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) {
                                ipv6_cpy(v6->runtime_opts_v6.dns[0], v6->gateway);
                            }
                        }

                        if (p.has_pd) {
                            ipv6_cpy(v6->runtime_opts_v6.pd_prefix, p.pd_prefix);
                            v6->runtime_opts_v6.pd_prefix_len = p.pd_prefix_len;
                            v6->runtime_opts_v6.pd_preferred_lft = p.pd_preferred_lft;
                            v6->runtime_opts_v6.pd_valid_lft = p.pd_valid_lft;
                        }

                        if (p.t1) v6->runtime_opts_v6.t1 = p.t1;
                        if (p.t2) v6->runtime_opts_v6.t2 = p.t2;

                        if (p.has_addr) {
                            uint8_t gw[16];

                            if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) ipv6_cpy(gw, v6->gateway);
                            else memset(gw, 0, 16);

                            (void)l3_ipv6_update(v6->l3_id, p.addr, 128, gw, IPV6_CFG_DHCPV6, v6->kind);
                            b->target_generation = v6->generation;

                            uint32_t lease_s = p.valid_lft;
                            v6->runtime_opts_v6.lease = lease_s;
                            v6->runtime_opts_v6.lease_start_time = get_time();

                            uint32_t t1_s = v6->runtime_opts_v6.t1;
                            uint32_t t2_s = v6->runtime_opts_v6.t2;

                            if (!t1_s) t1_s = lease_s / 2;
                            if (!t2_s) t2_s = (lease_s / 8) * 7;

                            b->t1_left_ms = t1_s * 1000u;
                            b->t2_left_ms = t2_s * 1000u;
                            b->lease_left_ms = lease_s * 1000u;
                        }

                        v6->dhcpv6_state = DHCPV6_S_BOUND;
                        reset_backoff(b);
                        b->last_tick_ms = (uint32_t)get_time();
                    } else if (v6->dhcpv6_state == DHCPV6_S_SOLICIT) {
                        if (p.has_server_id) {
                            v6->runtime_opts_v6.server_id_len = p.server_id_len;
                            if (p.server_id_len) memcpy(v6->runtime_opts_v6.server_id, p.server_id, p.server_id_len);
                        }

                        if (p.has_dns) memcpy(v6->runtime_opts_v6.dns, p.dns, sizeof(v6->runtime_opts_v6.dns));
                        if (p.has_ntp) memcpy(v6->runtime_opts_v6.ntp, p.ntp, sizeof(v6->runtime_opts_v6.ntp));
                        int has_dns = !ipv6_is_unspecified(v6->runtime_opts_v6.dns[0]) || !ipv6_is_unspecified(v6->runtime_opts_v6.dns[1]);

                        if (!has_dns) {
                            if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) {
                                ipv6_cpy(v6->runtime_opts_v6.dns[0], v6->gateway);
                            }
                        }

                        if (p.has_pd) {
                            ipv6_cpy(v6->runtime_opts_v6.pd_prefix, p.pd_prefix);
                            v6->runtime_opts_v6.pd_prefix_len = p.pd_prefix_len;
                            v6->runtime_opts_v6.pd_preferred_lft = p.pd_preferred_lft;
                            v6->runtime_opts_v6.pd_valid_lft = p.pd_valid_lft;
                        }

                        if (p.t1) v6->runtime_opts_v6.t1 = p.t1;
                        if (p.t2) v6->runtime_opts_v6.t2 = p.t2;

                        if (p.has_addr) {
                            uint8_t gw[16];

                            if (!ipv6_is_unspecified(v6->gateway) && !ipv6_is_multicast(v6->gateway)) ipv6_cpy(gw, v6->gateway);
                            else memset(gw, 0, 16);

                            (void)l3_ipv6_update(v6->l3_id, p.addr, 128, gw, IPV6_CFG_DHCPV6, v6->kind);
                            b->target_generation = v6->generation;

                            uint32_t lease_s = p.valid_lft;
                            v6->runtime_opts_v6.lease = lease_s;
                            v6->runtime_opts_v6.lease_start_time = get_time();

                            uint32_t t1_s = v6->runtime_opts_v6.t1;
                            uint32_t t2_s = v6->runtime_opts_v6.t2;

                            if (!t1_s) t1_s = lease_s / 2;
                            if (!t2_s) t2_s = (lease_s / 8) * 7;

                            b->t1_left_ms = t1_s * 1000u;
                            b->t2_left_ms = t2_s * 1000u;
                            b->lease_left_ms = lease_s * 1000u;
                        }

                        v6->dhcpv6_state = DHCPV6_S_BOUND;
                        reset_backoff(b);
                        b->last_tick_ms = (uint32_t)get_time();
                    } else if (v6->dhcpv6_state == DHCPV6_S_RELEASING || v6->dhcpv6_state == DHCPV6_S_DECLINING) {
                        reset_lease_state(v6, b);
                    }
                }
            }
        }
    }

    b->retry_left_ms = next_backoff_ms(b);
    b->last_tick_ms = (uint32_t)get_time();
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
                if (stateful || (stateless && !v6->dhcpv6_stateless_done)) {
                    active_work = true;
                    break;
                }
            }
        }
        if (!active_work) break;

        uint32_t sleep_ms = g_dhcpv6_rekick ? 10 : 2500;
        if (g_dhcpv6_binds) {
            for (linked_list_node_t* it = g_dhcpv6_binds->head; it; it = it->next) {
                dhcpv6_bind_t* b = (dhcpv6_bind_t*)it->data;
                if (!b || b->done || !b->sock) continue;
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(b->target_l3_id);
                if (!v6 || !v6->l2 || !v6->l2->is_up) continue;

                uint32_t wait_ms = 100;
                if (b->retry_left_ms) {
                    wait_ms = b->retry_left_ms;
                } else if (v6->dhcpv6_state == DHCPV6_S_BOUND) {
                    wait_ms = 2500;
                    if (b->t1_left_ms && b->t1_left_ms < wait_ms) wait_ms = b->t1_left_ms;
                    if (b->t2_left_ms && b->t2_left_ms < wait_ms) wait_ms = b->t2_left_ms;
                    if (b->lease_left_ms && b->lease_left_ms < wait_ms) wait_ms = b->lease_left_ms;
                } else if (v6->dhcpv6_state == DHCPV6_S_RENEWING || v6->dhcpv6_state == DHCPV6_S_REBINDING) {
                    wait_ms = 100;
                    if (v6->dhcpv6_state == DHCPV6_S_RENEWING && b->t2_left_ms && b->t2_left_ms < wait_ms) wait_ms = b->t2_left_ms;
                    if (b->lease_left_ms && b->lease_left_ms < wait_ms) wait_ms = b->lease_left_ms;
                }
                if (wait_ms < 10) wait_ms = 10;
                if (wait_ms > 2500) wait_ms = 2500;
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