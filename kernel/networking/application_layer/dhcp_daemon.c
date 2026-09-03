#include "dhcp_daemon.h"

#include "console/kio.h"
#include "std/memory.h"
#include "process/scheduler.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"
#include "random/random.h"

#include "networking/application_layer/dhcp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "net/network_types.h"
#include "networking/link_layer/arp.h"
#include "networking/link_layer/link_utils.h"
#include "networking/transport_layer/udp.h"

#include "networking/transport_layer/csocket.h"
#include "networking/transport_layer/trans_utils.h"

#include "types.h"
#include "networking/interface_manager.h"
#include "networking/network.h"
#include "syscalls/syscalls.h"
//TODO this code is a mess. cleanup
typedef enum {
    DHCP_S_INIT = 0,
    DHCP_S_SELECTING,
    DHCP_S_REQUESTING,
    DHCP_S_BOUND,
    DHCP_S_RENEWING,
    DHCP_S_REBINDING
} dhcp_state_t;

typedef enum {
    DHCP_APPLY_FAILED = 0,
    DHCP_APPLY_OK,
    DHCP_APPLY_CONFLICT
} dhcp_apply_result_t;

typedef struct {
    uint8_t ifindex;
    l3_id_t l3_id;
    uint32_t seen_epoch;
    uint32_t seen_generation;
    ipv4_cfg_t mode;
    dhcp_state_t state;
    uint32_t t1_left_ms;
    uint32_t t2_left_ms;
    uint32_t lease_left_ms;
    uint32_t last_tick_ms;
    uint32_t last_xid;
    uint32_t trans_xid;
    uint32_t server_ip_net;
    uint8_t mac[MAC_ADDR_LEN];
    bool mac_ok;
    bool needs_inform;
    socket_handle_t sock;
    uint32_t retry_left_ms;
    uint32_t backoff_ms;
} dhcp_if_state_t;

static volatile bool g_force_renew = false;
static volatile uint8_t g_dhcp_running;
static volatile uint8_t g_dhcp_pending;
static volatile uint8_t g_dhcp_rekick;
static dhcp_if_state_t g_if[MAX_IPV4_L3_INTERFACES];
static int g_if_count = 0;

void dhcp_force_renew() {
    g_force_renew = true;
    dhcp_daemon_kick();
}

static uint32_t dhcp_next_backoff_ms(dhcp_if_state_t* st) {
    if (st->backoff_ms == 0) st->backoff_ms = 4000;
    else {
        uint64_t v = (uint64_t)st->backoff_ms * 2u;
        if (v > 64000u) v = 64000u;
        st->backoff_ms = (uint32_t)v;
    }
    rng_t rng;
    rng_init_random(&rng);
    uint32_t jitter = (uint32_t)(rng_next32(&rng) % 2000u);
    int32_t signed_jitter = (int32_t)jitter - 1000;
    int64_t val = (int64_t)st->backoff_ms + signed_jitter;
    if (val < 1000) val = 1000;
    return (uint32_t)val;
}

static void dhcp_reset_backoff(dhcp_if_state_t* st) {
    st->backoff_ms = 0;
    st->retry_left_ms = 0;
}

static void ensure_inventory() {
    for (int i = 0; i < g_if_count;) {
        dhcp_if_state_t* st = &g_if[i];
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
        if (!ipv4_l3_is_active(v4) || v4->generation != st->seen_generation) {
            if (st->sock) close_socket(st->sock);
            if (i < g_if_count - 1) g_if[i] = g_if[g_if_count - 1];
            g_if_count--;
            continue;
        }
        i++;
    }
    uint8_t n = l2_interface_count();
    for (uint8_t ix = 0; ix < n; ix++) {
        l2_interface_t* l2 = l2_interface_at(ix);
        if (!l2 || !l2->is_up) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!ipv4_l3_is_active(v4)) continue;
            bool found = false;
            for (int i = 0; i < g_if_count; i++) {
                if (g_if[i].l3_id == v4->l3_id) {
                    found = true;
                    break;
                }
            }
            if (found || g_if_count >= MAX_IPV4_L3_INTERFACES) continue;
            dhcp_if_state_t* st = &g_if[g_if_count++];
            memset(st, 0, sizeof(*st));
            st->ifindex = l2->ifindex;
            st->l3_id = v4->l3_id;
            st->seen_epoch = v4->epoch;
            st->seen_generation = v4->generation;
            st->mode = v4->mode;
            st->needs_inform = v4->mode == IPV4_CFG_STATIC && v4->ip != 0;
            st->last_tick_ms = (uint32_t)get_time();
            if (v4->mode == IPV4_CFG_DHCP && v4->ip && v4->runtime_opts_v4.lease && v4->runtime_opts_v4.lease_start_time) {
                st->state = DHCP_S_BOUND;
                if (v4->runtime_opts_v4.server_ip) st->server_ip_net = bswap32(v4->runtime_opts_v4.server_ip);
            }

            const uint8_t* m = network_get_mac(st->ifindex);
            if (m) {
                mac_copy(st->mac, m);
                st->mac_ok = true;
            }
        }
    }
    
    for (int i = 0; i < g_if_count; i++) {
        dhcp_if_state_t* st = &g_if[i];
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
        if (!ipv4_l3_is_active(v4)) continue;

        if (st->mode != v4->mode) {
            st->mode = v4->mode;
            st->state = DHCP_S_INIT;
            st->t1_left_ms = 0;
            st->t2_left_ms = 0;
            st->lease_left_ms = 0;
            st->last_xid = 0;
            st->trans_xid = 0;
            st->server_ip_net = 0;
            st->retry_left_ms = 0;
            st->backoff_ms = 0;
            st->last_tick_ms = get_time();
            st->needs_inform = v4->mode == IPV4_CFG_STATIC && v4->ip != 0;
        } else if (st->seen_epoch != v4->epoch && v4->mode == IPV4_CFG_STATIC && v4->ip) st->needs_inform = true;
        st->seen_epoch = v4->epoch;
        st->seen_generation = v4->generation;

        bool needs_socket = v4->mode == IPV4_CFG_DHCP || st->needs_inform;
        if (needs_socket && !st->sock) {
            st->sock = create_socket(PROTO_UDP, &(SocketOptions){.flags = SOCK_OPT_NONBLOCK});
            if (st->sock) {
                SockBindSpec spec = {.kind = BIND_L3, .ver = IP_VER4, .l3_id = st->l3_id};
                if (set_socket_option(st->sock, SOCK_OPT_BROADCAST_ALLOWED, NULL, 0) != SOCK_OK || bind_socket(st->sock, &spec, 68) != SOCK_OK) {
                    close_socket(st->sock);
                    st->sock = 0;
                }
            }
        } else if (!needs_socket && st->sock) {
            close_socket(st->sock);
            st->sock = 0;
        }
    }
}

static uint32_t udp_wait_for_type_on(socket_handle_t sock, uint8_t wanted, uint32_t expect_xid, const uint8_t mac[MAC_ADDR_LEN], dhcp_packet* out, uint32_t timeout_ms) {
    if (!out) return 0;

    uint32_t waited = 0;
    while(waited < timeout_ms){
        net_l4_endpoint src;
        memset(&src, 0, sizeof(src));
        int64_t r = receive_from_socket(sock, out, sizeof(*out), &src);
        if (r > 0) {
            if (src.port != 67) continue;
            if ((size_t)r < sizeof(dhcp_packet) - sizeof(out->options) + 4) continue;
            if (out->htype != 1 || out->hlen != MAC_ADDR_LEN) continue;
            if (!dhcp_has_valid_cookie(out)) continue;
            if (expect_xid && out->xid != expect_xid) continue;
            if (mac && !mac_equal(out->chaddr, mac)) continue;
            uint16_t idx = dhcp_parse_option_bounded(out, (uint32_t)r, 53);
            if (idx == UINT16_MAX || out->options[idx+1] < 1) continue;
            if (out->options[idx+2] != wanted) continue;
            return (uint32_t)r;
        }
        msleep(50);
        waited += 50;
    }
    return 0;
}

static uint32_t udp_wait_for_ack_or_nak(socket_handle_t sock, uint32_t expect_xid, const uint8_t mac[MAC_ADDR_LEN], dhcp_packet* out, uint32_t timeout_ms, uint8_t *out_msg_type) {
    if (!out) return 0;

    uint32_t waited = 0;
    while (waited < timeout_ms) {
        net_l4_endpoint src;
        memset(&src, 0, sizeof(src));
        int64_t r = receive_from_socket(sock, out, sizeof(*out), &src);
        if (r > 0) {
            if (src.port != 67) continue;
            if ((size_t)r < sizeof(dhcp_packet) - sizeof(out->options) + 4) continue;
            if (out->htype != 1 || out->hlen != MAC_ADDR_LEN) continue;
            if (!dhcp_has_valid_cookie(out)) continue;
            if (expect_xid && out->xid != expect_xid) continue;
            if (mac && !mac_equal(out->chaddr, mac)) continue;
            uint16_t idx = dhcp_parse_option_bounded(out, (uint32_t)r, 53);
            if (idx == UINT16_MAX || out->options[idx+1] < 1) continue;
            uint8_t mtype = out->options[idx+2];
            if (mtype != DHCPACK && mtype != DHCPNAK) continue;
            if (out_msg_type) *out_msg_type = mtype;
            return (uint32_t)r;
        }
        msleep(50);
        waited += 50;
    }
    return 0;
}

static void dhcp_send_decline_for(dhcp_if_state_t* st, uint32_t offered_ip_net) {
    if (!st || !st->sock || !st->server_ip_net || !offered_ip_net) return;

    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) mac_copy(req.mac, st->mac);
    req.offered_ip = offered_ip_net;
    req.server_ip = st->server_ip_net;

    dhcp_packet pkt;
    uint32_t pkt_len = dhcp_build_packet(&req, DHCPDECLINE, st->last_xid, DHCPK_DECLINE, true, &pkt);
    if (!pkt_len) return;

    uint32_t bcast = IPV4_LIMITED_BROADCAST;
    net_l4_endpoint dst;
    make_ep(&bcast, 67, IP_VER4, &dst);
    send_to_socket(st->sock, &dst, &pkt, pkt_len);
}

static dhcp_apply_result_t apply_offer_to_l3(l3_id_t l3_id, dhcp_packet *p, uint32_t payload_len, uint32_t xid, dhcp_if_state_t* st) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    if (!v4) return DHCP_APPLY_FAILED;
    net_runtime_opts_t rt_local;
    memset(&rt_local, 0, sizeof(rt_local));
    uint32_t yi_net = p->yiaddr;
    uint32_t ip_host = bswap32(yi_net);
    uint16_t idx;
    uint8_t len;
    uint32_t mask_host = 0;

    idx = dhcp_parse_option_bounded(p, payload_len, 1);
    if (idx != UINT16_MAX && (len = p->options[idx+1]) >= 4) {
        uint32_t mask_net;
        memcpy(&mask_net, &p->options[idx+2], 4);
        mask_host = bswap32(mask_net);
    }

    uint32_t gw_host = 0;
    idx = dhcp_parse_option_bounded(p, payload_len, 3);
    if (idx != UINT16_MAX && (len = p->options[idx+1]) >= 4) {
        uint32_t gw_net;
        memcpy(&gw_net, &p->options[idx+2], 4);
        gw_host = bswap32(gw_net);
    }

    idx = dhcp_parse_option_bounded(p, payload_len, 6);
    if (idx != UINT16_MAX) {
        len = p->options[idx+1];
        for (int i = 0; i < 2 && (i*4 + 4) <= len; i++) {
            uint32_t dns_net;
            memcpy(&dns_net, &p->options[idx+2 + i*4], 4);
            rt_local.dns[i] = bswap32(dns_net);
        }
    }

    idx = dhcp_parse_option_bounded(p, payload_len, 42);
    if (idx != UINT16_MAX) {
        len = p->options[idx+1];
        for (int i = 0; i < 2 && (i*4 + 4) <= len; i++) {
            uint32_t ntp_net;
            memcpy(&ntp_net, &p->options[idx+2 + i*4], 4);
            rt_local.ntp[i] = bswap32(ntp_net);
        }
    }

    idx = dhcp_parse_option_bounded(p, payload_len, 26);
    if (idx != UINT16_MAX && p->options[idx+1] == 2) {
        uint16_t mtu_net;
        memcpy(&mtu_net, &p->options[idx+2], 2);
        uint16_t mtu = bswap16(mtu_net);
        if (mtu >= 68) rt_local.mtu = mtu;
    }

    uint32_t lease_s = 0;
    idx = dhcp_parse_option_bounded(p, payload_len, 51);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t lease_net;
        memcpy(&lease_net, &p->options[idx+2], 4);
        lease_s = bswap32(lease_net);
        rt_local.lease = lease_s;
    }

    uint32_t t1_s = 0;
    idx = dhcp_parse_option_bounded(p, payload_len, 58);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t t1_net;
        memcpy(&t1_net, &p->options[idx+2], 4);
        t1_s = bswap32(t1_net);
    } else {
        if (lease_s) t1_s = lease_s / 2;
    }
    rt_local.t1 = t1_s;

    uint32_t t2_s = 0;
    idx = dhcp_parse_option_bounded(p, payload_len, 59);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t t2_net;
        memcpy(&t2_net, &p->options[idx+2], 4);
        t2_s = bswap32(t2_net);
    } else {
        if (lease_s) t2_s = (lease_s / 8) * 7;
    }
    rt_local.t2 = t2_s;

    idx = dhcp_parse_option_bounded(p, payload_len, 54);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        memcpy(&st->server_ip_net, &p->options[idx+2], 4);
    }
    if (st->server_ip_net) rt_local.server_ip = bswap32(st->server_ip_net);

    if (rt_local.dns[0] == 0 && gw_host != 0) rt_local.dns[0] = gw_host;
    rt_local.xid = (uint16_t)xid;
    rt_local.lease_start_time = get_time();

    if (ip_host != v4->ip && !arp_dad_ipv4_on(st->ifindex, ip_host)) {
        dhcp_send_decline_for(st, yi_net);
        return DHCP_APPLY_CONFLICT;
    }

    if (!l3_ipv4_update(l3_id, ip_host, mask_host, gw_host, IPV4_CFG_DHCP, &rt_local)) return DHCP_APPLY_FAILED;

    st->t1_left_ms = t1_s * 1000;
    st->t2_left_ms = t2_s * 1000;
    st->lease_left_ms = lease_s * 1000;
    st->last_tick_ms = get_time();
    l3_ipv4_interface_t* updated = l3_ipv4_find_by_id(l3_id);
    if (updated) {
        st->seen_epoch = updated->epoch;
        st->seen_generation = updated->generation;
    }
    return DHCP_APPLY_OK;
}

static void dhcp_send_discover_for(dhcp_if_state_t* st) {
    rng_t rng;
    rng_init_random(&rng);
    uint32_t xid = rng_next32(&rng);
    st->trans_xid = xid;
    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) mac_copy(req.mac, st->mac);
    dhcp_packet pkt;
    uint32_t pkt_len = dhcp_build_packet(&req, DHCPDISCOVER, xid, DHCPK_DISCOVER, true, &pkt);
    if (!pkt_len) return;
    uint32_t bcast = IPV4_LIMITED_BROADCAST;
    net_l4_endpoint dst;
    make_ep(&bcast, 67, IP_VER4, &dst);
    send_to_socket(st->sock, &dst, &pkt, pkt_len);
}

static void dhcp_send_renew_for(dhcp_if_state_t* st) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
    if (!v4) return;
    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) mac_copy(req.mac, st->mac);
    uint32_t ip_net = bswap32(v4->ip);
    req.offered_ip = ip_net;
    req.server_ip = st->server_ip_net;
    rng_t rng;
    rng_init_random(&rng);
    st->trans_xid = rng_next32(&rng);
    dhcp_packet pkt;
    uint32_t pkt_len = dhcp_build_packet(&req, DHCPREQUEST, st->trans_xid, DHCPK_RENEW, st->server_ip_net == 0, &pkt);
    if (!pkt_len) return;
    uint32_t dip = st->server_ip_net ? bswap32(st->server_ip_net) : IPV4_LIMITED_BROADCAST;
    net_l4_endpoint dst;
    make_ep(&dip, 67, IP_VER4, &dst);
    send_to_socket(st->sock, &dst, &pkt, pkt_len);
}

static void dhcp_send_rebind_for(dhcp_if_state_t* st) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
    if (!v4) return;
    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) mac_copy(req.mac, st->mac);
    uint32_t ip_net = bswap32(v4->ip);
    req.offered_ip = ip_net;
    req.server_ip = 0;
    rng_t rng;
    rng_init_random(&rng);
    st->trans_xid = rng_next32(&rng);
    dhcp_packet pkt;
    uint32_t pkt_len = dhcp_build_packet(&req, DHCPREQUEST, st->trans_xid, DHCPK_REBIND, true, &pkt);
    if (!pkt_len) return;
    uint32_t dip = IPV4_LIMITED_BROADCAST;
    net_l4_endpoint dst;
    make_ep(&dip, 67, IP_VER4, &dst);
    send_to_socket(st->sock, &dst, &pkt, pkt_len);
}

static void dhcp_send_inform_for(dhcp_if_state_t* st) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
    if (!v4 || !v4->ip) return;
    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) mac_copy(req.mac, st->mac);
    uint32_t ip_net = bswap32(v4->ip);
    req.offered_ip = ip_net;
    req.server_ip = 0;
    rng_t rng;
    rng_init_random(&rng);
    uint32_t xid = rng_next32(&rng);
    dhcp_packet pkt;
    uint32_t pkt_len = dhcp_build_packet(&req, DHCPINFORM, xid, DHCPK_INFORM, true, &pkt);
    if (!pkt_len) return;
    uint32_t dip = IPV4_LIMITED_BROADCAST;
    net_l4_endpoint dst;
    make_ep(&dip, 67, IP_VER4, &dst);
    send_to_socket(st->sock, &dst, &pkt, pkt_len);
}

static void dhcp_drop_lease_and_retry(dhcp_if_state_t* st) {
    l3_ipv4_update(st->l3_id, 0, 0, 0, IPV4_CFG_DHCP, NULL);
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
    if (v4) {
        st->seen_epoch = v4->epoch;
        st->seen_generation = v4->generation;
    }
    st->t1_left_ms = 0;
    st->t2_left_ms = 0;
    st->lease_left_ms = 0;
    st->server_ip_net = 0;
    st->state = DHCP_S_INIT;
    st->retry_left_ms = dhcp_next_backoff_ms(st);
    st->last_tick_ms = (uint32_t)get_time();
}

static void fsm_once_for(dhcp_if_state_t* st, bool force_renew) {
    switch (st->state) {
    case DHCP_S_INIT: {
        if (st->retry_left_ms != 0) break;
        dhcp_send_discover_for(st);
        st->last_xid = st->trans_xid;
        st->state = DHCP_S_SELECTING;
    } break;
    case DHCP_S_SELECTING: {
        dhcp_packet offer;
        uint32_t offer_len = udp_wait_for_type_on(st->sock, DHCPOFFER, st->last_xid, st->mac_ok ? st->mac : NULL, &offer, 5000);
        if (!offer_len) {
            st->state = DHCP_S_INIT;
            st->retry_left_ms = dhcp_next_backoff_ms(st);
            st->last_tick_ms = (uint32_t)get_time();
            break;
        }
        dhcp_request req;
        memset(&req, 0, sizeof(req));
        if (st->mac_ok) mac_copy(req.mac, st->mac);
        uint16_t idx54 = dhcp_parse_option_bounded(&offer, offer_len, 54);
        if (idx54 != UINT16_MAX && offer.options[idx54+1] >= 4) memcpy(&st->server_ip_net, &offer.options[idx54+2], 4);
        memcpy(&req.offered_ip, &offer.yiaddr, 4);
        req.server_ip = st->server_ip_net;
        dhcp_packet pkt;
        uint32_t pkt_len = dhcp_build_packet(&req, DHCPREQUEST, st->trans_xid, DHCPK_SELECT, true, &pkt);
        if (!pkt_len) {
            st->state = DHCP_S_INIT;
            st->retry_left_ms = dhcp_next_backoff_ms(st);
            st->last_tick_ms = (uint32_t)get_time();
            break;
        }

        uint32_t dip = IPV4_LIMITED_BROADCAST;
        net_l4_endpoint dst;
        make_ep(&dip, 67, IP_VER4, &dst);
        send_to_socket(st->sock, &dst, &pkt, pkt_len);
        st->state = DHCP_S_REQUESTING;
        dhcp_reset_backoff(st);
    } break;
    case DHCP_S_REQUESTING: {
        dhcp_packet resp;
        uint8_t mtype = 0;
        uint32_t resp_len = udp_wait_for_ack_or_nak(st->sock, st->last_xid, st->mac_ok ? st->mac : NULL, &resp, 5000, &mtype);
        if (!resp_len) {
            st->state = DHCP_S_INIT;
            st->retry_left_ms = dhcp_next_backoff_ms(st);
            st->last_tick_ms = (uint32_t)get_time();
        } else if (mtype == DHCPACK) {
            dhcp_apply_result_t applied = apply_offer_to_l3(st->l3_id, &resp, resp_len, st->last_xid, st);
            if (applied == DHCP_APPLY_OK) {
                st->state = DHCP_S_BOUND;
                dhcp_reset_backoff(st);
            } else {
                dhcp_drop_lease_and_retry(st);
                if (applied == DHCP_APPLY_CONFLICT && st->retry_left_ms < 10000) st->retry_left_ms = 10000;
            }
        } else {
            dhcp_drop_lease_and_retry(st);
        }
    } break;
    case DHCP_S_BOUND: {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
        if (v4 && v4->runtime_opts_v4.lease && !st->lease_left_ms) {
            dhcp_drop_lease_and_retry(st);
        } else if (force_renew || (!st->t1_left_ms && st->lease_left_ms)) {
            st->state = DHCP_S_RENEWING;
            dhcp_reset_backoff(st);
        }
    } break;
    case DHCP_S_RENEWING:
    case DHCP_S_REBINDING: {
        bool rebindin = st->state == DHCP_S_REBINDING;
        if (!st->lease_left_ms) {
            dhcp_drop_lease_and_retry(st);
            break;
        }
        if (!rebindin && !st->t2_left_ms) {
            st->state = DHCP_S_REBINDING;
            dhcp_reset_backoff(st);
            rebindin = true;
        }
        if (st->retry_left_ms) break;

        if (rebindin) dhcp_send_rebind_for(st);
        else dhcp_send_renew_for(st);
        st->last_xid = st->trans_xid;

        dhcp_packet p;
        uint8_t mtype = 0;
        uint32_t payload_len = udp_wait_for_ack_or_nak(st->sock, st->last_xid, st->mac_ok ? st->mac : NULL, &p, 2000, &mtype);
        if (payload_len) {
            if (mtype == DHCPACK) {
                dhcp_apply_result_t applied = apply_offer_to_l3(st->l3_id, &p, payload_len, st->last_xid, st);
                if (applied == DHCP_APPLY_OK) {
                    st->state = DHCP_S_BOUND;
                    dhcp_reset_backoff(st);
                } else {
                    dhcp_drop_lease_and_retry(st);
                    if (applied == DHCP_APPLY_CONFLICT && st->retry_left_ms < 10000) st->retry_left_ms = 10000;
                }
            } else {
                dhcp_drop_lease_and_retry(st);
            }
        } else {
            st->retry_left_ms = dhcp_next_backoff_ms(st);
            st->last_tick_ms = (uint32_t)get_time();
        }
    } break;
    }
}

static void tick_timers() {
    uint32_t now_ms = (uint32_t)get_time();
    for (int i = 0; i < g_if_count; i++) {
        uint32_t elapsed_ms = now_ms - g_if[i].last_tick_ms;
        g_if[i].last_tick_ms = now_ms;
        if (g_if[i].retry_left_ms > elapsed_ms) g_if[i].retry_left_ms -= elapsed_ms; else g_if[i].retry_left_ms = 0;

        if (g_if[i].state != DHCP_S_BOUND && g_if[i].state != DHCP_S_RENEWING && g_if[i].state != DHCP_S_REBINDING) continue;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(g_if[i].l3_id);
        if (!v4 || !v4->runtime_opts_v4.lease || !v4->runtime_opts_v4.lease_start_time) continue;

        uint32_t lease_ms = v4->runtime_opts_v4.lease * 1000;
        uint32_t t1_ms = v4->runtime_opts_v4.t1 * 1000;
        uint32_t t2_ms = v4->runtime_opts_v4.t2 * 1000;
        uint32_t age_ms = now_ms - v4->runtime_opts_v4.lease_start_time;
        g_if[i].lease_left_ms = age_ms < lease_ms ? lease_ms - age_ms : 0;
        g_if[i].t1_left_ms = age_ms < t1_ms ? t1_ms - age_ms : 0;
        g_if[i].t2_left_ms = age_ms < t2_ms ? t2_ms - age_ms : 0;
    }
}

static void maybe_send_inform() {
    for (int i = 0; i < g_if_count; i++) {
        if (!g_if[i].needs_inform) continue;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(g_if[i].l3_id);
        if (!v4 || v4->mode != IPV4_CFG_STATIC || !v4->ip) { g_if[i].needs_inform = false; continue; }
        dhcp_send_inform_for(&g_if[i]);
        g_if[i].needs_inform = false;
        if (g_if[i].sock) {
            close_socket(g_if[i].sock); 
            g_if[i].sock = 0;
        }
    }
}

int dhcp_daemon_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;

    irq_flags_t irq = irq_save_disable();
    g_dhcp_pending = 0;
    g_dhcp_running = 1;
    g_dhcp_rekick = 0;
    irq_restore(irq);

    for (;;) {
        irq = irq_save_disable();
        bool force_renew = g_force_renew;
        g_force_renew = false;
        g_dhcp_rekick = 0;
        irq_restore(irq);

        ensure_inventory();
        tick_timers();

        for (int i = 0; i < g_if_count; i++) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(g_if[i].l3_id);
            if (!v4 || v4->mode != IPV4_CFG_DHCP || !g_if[i].sock) continue;
            fsm_once_for(&g_if[i], force_renew);
        }
        maybe_send_inform();

        bool active_work = g_force_renew;
        for (int i = 0; i < g_if_count && !active_work; i++) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(g_if[i].l3_id);
            if (!ipv4_l3_is_active(v4)) continue;
            if (v4->mode == IPV4_CFG_DHCP || g_if[i].needs_inform) active_work = true;
        }
        if (!active_work) break;

        uint32_t sleep_ms = 2500;
        if (g_dhcp_rekick || g_force_renew) sleep_ms = 10;
        for (int i = 0; i < g_if_count; i++) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(g_if[i].l3_id);
            if (!ipv4_l3_is_active(v4)) continue;
            if (g_if[i].needs_inform) {
                sleep_ms = 10;
                break;
            }
            if (v4->mode != IPV4_CFG_DHCP) continue;

            uint32_t wait_ms = 100;
            if (g_if[i].retry_left_ms) {
                wait_ms = g_if[i].retry_left_ms;
            } else if (g_if[i].state == DHCP_S_BOUND) {
                wait_ms = 2500;
                if (g_if[i].t1_left_ms && g_if[i].t1_left_ms < wait_ms) wait_ms = g_if[i].t1_left_ms;
                if (g_if[i].t2_left_ms && g_if[i].t2_left_ms < wait_ms) wait_ms = g_if[i].t2_left_ms;
                if (g_if[i].lease_left_ms && g_if[i].lease_left_ms < wait_ms) wait_ms = g_if[i].lease_left_ms;
            } else if (g_if[i].state == DHCP_S_RENEWING || g_if[i].state == DHCP_S_REBINDING) {
                wait_ms = 100;
                if (g_if[i].state == DHCP_S_RENEWING && g_if[i].t2_left_ms && g_if[i].t2_left_ms < wait_ms) wait_ms = g_if[i].t2_left_ms;
                if (g_if[i].lease_left_ms && g_if[i].lease_left_ms < wait_ms) wait_ms = g_if[i].lease_left_ms;
            }
            if (wait_ms < 10) wait_ms = 10;
            if (wait_ms > 2500) wait_ms = 2500;
            if (wait_ms < sleep_ms) sleep_ms = wait_ms;
        }
        msleep(sleep_ms);
    }

    irq = irq_save_disable();
    g_dhcp_running = 0;
    bool rekick = g_dhcp_rekick != 0;
    g_dhcp_rekick = 0;
    irq_restore(irq);
    if (rekick) dhcp_daemon_kick();
    return 0;
}

void dhcp_daemon_kick(void) {
    irq_flags_t irq = irq_save_disable();
    if (g_dhcp_running || g_dhcp_pending) {
        g_dhcp_rekick = 1;
        irq_restore(irq);
        return;
    }
    irq_restore(irq);

    bool config_work = false;
    uint8_t n = l2_interface_count();
    for (uint8_t ix = 0; ix < n && !config_work; ix++) {
        l2_interface_t* l2 = l2_interface_at(ix);
        if (!l2 || !l2->is_up) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!ipv4_l3_is_active(v4)) continue;
            if (v4->mode == IPV4_CFG_DHCP || (v4->mode == IPV4_CFG_STATIC && v4->ip)) {
                config_work = true;
                break;
            }
        }
    }
    if (!g_force_renew && !config_work) return;

    irq = irq_save_disable();
    if (g_dhcp_running || g_dhcp_pending) {
        g_dhcp_rekick = 1;
        irq_restore(irq);
        return;
    }
    g_dhcp_pending = 1;
    irq_restore(irq);

    if (!create_kernel_process("dhcp_daemon", dhcp_daemon_entry, 0, 0)) {
        irq = irq_save_disable();
        g_dhcp_pending = 0;
        irq_restore(irq);
    }
}
