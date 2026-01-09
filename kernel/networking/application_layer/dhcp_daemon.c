#include "dhcp_daemon.h"

#include "console/kio.h"
#include "std/memory.h"
#include "process/scheduler.h"
#include "random/random.h"

#include "networking/application_layer/dhcp.h"
#include "networking/internet_layer/ipv4.h"
#include "net/network_types.h"
#include "networking/link_layer/arp.h"
#include "networking/transport_layer/udp.h"

#include "networking/transport_layer/csocket_udp.h"
#include "networking/transport_layer/trans_utils.h"

#include "types.h"
#include "networking/interface_manager.h"
#include "networking/network.h"
#include "networking/internet_layer/ipv4_route.h"
#include "syscalls/syscalls.h"

typedef enum {
    DHCP_S_INIT = 0,
    DHCP_S_SELECTING,
    DHCP_S_REQUESTING,
    DHCP_S_BOUND,
    DHCP_S_RENEWING,
    DHCP_S_REBINDING
} dhcp_state_t;

typedef struct {
    uint8_t ifindex;
    uint8_t l3_id;
    dhcp_state_t state;
    uint32_t t1_left_ms;
    uint32_t t2_left_ms;
    uint32_t lease_left_ms;
    uint32_t last_xid;
    uint32_t trans_xid;
    uint32_t server_ip_net;
    uint8_t mac[6];
    bool mac_ok;
    bool needs_inform;
    socket_handle_t sock;
    uint32_t retry_left_ms;
    uint32_t backoff_ms;
} dhcp_if_state_t;

static volatile bool g_force_renew = false;
static uint16_t g_pid_dhcpd = 0xFFFF;
static dhcp_if_state_t g_if[MAX_L2_INTERFACES * MAX_IPV4_PER_INTERFACE];
static int g_if_count = 0;

uint16_t dhcp_get_pid() { return g_pid_dhcpd; }
bool dhcp_is_running() { return g_pid_dhcpd != 0xFFFF; }
void dhcp_set_pid(uint16_t p){ g_pid_dhcpd = p;    }
void dhcp_force_renew() { g_force_renew = true; }

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

static bool find_state(uint8_t l3_id, int* idx) {
    for (int i = 0; i < g_if_count; i++) if (g_if[i].l3_id == l3_id) { *idx = i; return true; }
    return false;
}

static void remove_state_at(int i) {
    if (g_if[i].sock) {
        socket_close_udp(g_if[i].sock);
        socket_destroy_udp(g_if[i].sock);
        g_if[i].sock = 0;
    }
    if (i < g_if_count - 1) g_if[i] = g_if[g_if_count - 1];
    g_if_count--;
}

static void ensure_inventory() {
    for (int i = 0; i < g_if_count;) {
        uint8_t l3id = g_if[i].l3_id;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3id);
        if (!v4 || v4->mode == IPV4_CFG_DISABLED || !v4->l2 || !v4->l2->is_up) { remove_state_at(i); continue; }
        i++;
    }
    uint8_t n = l2_interface_count();
    for (uint8_t ix = 0; ix < n; ix++) {
        l2_interface_t* l2 = l2_interface_at(ix);
        if (!l2 || !l2->is_up) continue;
        for (int s = 0; s < MAX_IPV4_PER_INTERFACE; s++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[s];
            if (!v4) continue;
            if (v4->mode == IPV4_CFG_DISABLED) continue;
            int idx;
            if (find_state(v4->l3_id, &idx)) continue;
            dhcp_if_state_t st;
            memset(&st, 0, sizeof(st));
            st.ifindex = l2->ifindex;
            st.l3_id = v4->l3_id;
            st.state = DHCP_S_INIT;
            st.t1_left_ms = 0;
            st.t2_left_ms = 0;
            st.lease_left_ms = 0;
            st.last_xid = 0;
            st.trans_xid = 0;
            st.server_ip_net = 0;
            const uint8_t* m = network_get_mac(st.ifindex);
            if (m) { memcpy(st.mac, m, 6); st.mac_ok = true; }
            st.needs_inform = (v4->mode == IPV4_CFG_STATIC && v4->ip != 0);
            st.sock = udp_socket_create(SOCK_ROLE_SERVER, g_pid_dhcpd, NULL);
            SockBindSpec spec;
            memset(&spec, 0, sizeof(spec));
            spec.kind = BIND_L3;
            spec.l3_id = st.l3_id;
            if (socket_bind_udp_ex(st.sock, &spec, 68) != SOCK_OK) {
                socket_destroy_udp(st.sock);
                continue;
            }
            st.retry_left_ms = 0;
            st.backoff_ms = 0;
            g_if[g_if_count++] = st;
        }
    }
}

static bool packet_mac_matches(const dhcp_packet *p, const uint8_t mac[6]) {
    if (!mac) return true;
    for (int i = 0; i < 6; i++) if (p->chaddr[i] != mac[i]) return false;
    return true;
}

static bool udp_wait_for_type_on(socket_handle_t sock, uint8_t wanted, uint32_t expect_xid, const uint8_t mac[6], dhcp_packet **outp, sizedptr *outsp, uint32_t timeout_ms) {
    uint32_t waited = 0;
    while(waited < timeout_ms){
        uint8_t buf[1024];
        net_l4_endpoint src;
        memset(&src, 0, sizeof(src));
        int64_t r = socket_recvfrom_udp_ex(sock, buf, sizeof(buf), &src);
        if (r > 0) {
            if (src.port != 67) { continue; }
            if ((size_t)r < sizeof(dhcp_packet) - sizeof(((dhcp_packet*)0)->options) + 4) { continue; }
            dhcp_packet *p = (dhcp_packet*)buf;

            if (p->htype != 1) { continue; }
            if (p->hlen != 6) { continue; }
            if (!dhcp_has_valid_cookie(p)) { continue; }
            if (expect_xid && p->xid != expect_xid) { continue; }
            if (mac && !packet_mac_matches(p, mac)) { continue; }
            uint16_t idx = dhcp_parse_option_bounded(p, (uint32_t)r, 53);
            if (idx == UINT16_MAX) { continue; }
            uint8_t len = p->options[idx+1];
            if (len < 1) { continue; }
            if (p->options[idx+2] != wanted) { continue; }
            uintptr_t copy = (uintptr_t)malloc((uint32_t)r);
            memcpy((void*)copy, buf, (size_t)r);
            if (outp) *outp= (dhcp_packet*)copy;
            if (outsp) *outsp = (sizedptr){ copy, (uint32_t)r };
            return true;
        } else {
            msleep(50);
            waited += 50;
        }
    }
    return false;
}

static bool udp_wait_for_ack_or_nak(socket_handle_t sock, uint32_t expect_xid, const uint8_t mac[6], dhcp_packet **outp, sizedptr *outsp, uint32_t timeout_ms, uint8_t *out_msg_type) {
    uint32_t waited = 0;
    while (waited < timeout_ms) {
        uint8_t buf[1024];
        net_l4_endpoint src;
        memset(&src, 0, sizeof(src));
        int64_t r = socket_recvfrom_udp_ex(sock, buf, sizeof(buf), &src);
        if (r > 0) {
            if (src.port != 67) { continue; }
            if ((size_t)r < sizeof(dhcp_packet) - sizeof(((dhcp_packet*)0)->options) + 4) { continue; }
            dhcp_packet* p = (dhcp_packet*)buf;
            if (p->htype != 1 || p->hlen != 6) { continue; }
            if (!dhcp_has_valid_cookie(p)) { continue; }
            if (expect_xid && p->xid != expect_xid) { continue; }
            if (mac && !packet_mac_matches(p, mac)) { continue; }
            uint16_t idx = dhcp_parse_option_bounded(p, (uint32_t)r, 53);
            if (idx == UINT16_MAX || p->options[idx+1] < 1) { continue; }
            uint8_t mtype = p->options[idx+2];
            if (mtype != DHCPACK && mtype != DHCPNAK) { continue; }
            uintptr_t copy = (uintptr_t)malloc((uint32_t)r);
            memcpy((void*)copy, buf, (size_t)r);
            if (outp) *outp = (dhcp_packet*)copy;
            if (outsp) *outsp = (sizedptr){ copy, (uint32_t)r };
            if (out_msg_type) *out_msg_type = mtype;
            return true;
        } else {
            msleep(50);
            waited += 50;
        }
    }
    return false;
}

static void apply_offer_to_l3(uint8_t ifindex, uint8_t l3_id, dhcp_packet *p, sizedptr sp, uint32_t xid, dhcp_if_state_t* st) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    if (!v4) return;
    net_runtime_opts_t rt_local;
    memset(&rt_local, 0, sizeof(rt_local));
    uint32_t yi_net = p->yiaddr;
    uint32_t ip_host = bswap32(yi_net);
    uint16_t idx;
    uint8_t len;
    uint32_t mask_host = 0;

    idx = dhcp_parse_option_bounded(p, sp.size, 1);
    if (idx != UINT16_MAX && (len = p->options[idx+1]) >= 4) {
        uint32_t mask_net;
        memcpy(&mask_net, &p->options[idx+2], 4);
        mask_host = bswap32(mask_net);
    }

    uint32_t gw_host = 0;
    idx = dhcp_parse_option_bounded(p, sp.size, 3);
    if (idx != UINT16_MAX && (len = p->options[idx+1]) >= 4) {
        uint32_t gw_net;
        memcpy(&gw_net, &p->options[idx+2], 4);
        gw_host = bswap32(gw_net);
    }

    idx = dhcp_parse_option_bounded(p, sp.size, 6);
    if (idx != UINT16_MAX) {
        len = p->options[idx+1];
        for (int i = 0; i < 2 && (i*4 + 4) <= len; i++) {
            uint32_t dns_net;
            memcpy(&dns_net, &p->options[idx+2 + i*4], 4);
            rt_local.dns[i] = bswap32(dns_net);
        }
    }

    idx = dhcp_parse_option_bounded(p, sp.size, 42);
    if (idx != UINT16_MAX) {
        len = p->options[idx+1];
        for (int i = 0; i < 2 && (i*4 + 4) <= len; i++) {
            uint32_t ntp_net;
            memcpy(&ntp_net, &p->options[idx+2 + i*4], 4);
            rt_local.ntp[i] = bswap32(ntp_net);
        }
    }

    idx = dhcp_parse_option_bounded(p, sp.size, 26);
    if (idx != UINT16_MAX && p->options[idx+1] == 2) {
        uint16_t mtu_net;
        memcpy(&mtu_net, &p->options[idx+2], 2);
        rt_local.mtu = bswap16(mtu_net);
    }

    uint32_t lease_s = 0;
    idx = dhcp_parse_option_bounded(p, sp.size, 51);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t lease_net;
        memcpy(&lease_net, &p->options[idx+2], 4);
        lease_s = bswap32(lease_net);
        rt_local.lease = lease_s;
    }

    uint32_t t1_s = 0;
    idx = dhcp_parse_option_bounded(p, sp.size, 58);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t t1_net;
        memcpy(&t1_net, &p->options[idx+2], 4);
        t1_s = bswap32(t1_net);
    } else {
        if (lease_s) t1_s = lease_s / 2;
    }
    rt_local.t1 = t1_s;

    uint32_t t2_s = 0;
    idx = dhcp_parse_option_bounded(p, sp.size, 59);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t t2_net;
        memcpy(&t2_net, &p->options[idx+2], 4);
        t2_s = bswap32(t2_net);
    } else {
        if (lease_s) t2_s = (lease_s / 8) * 7;
    }
    rt_local.t2 = t2_s;

    idx = dhcp_parse_option_bounded(p, sp.size, 54);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        memcpy(&st->server_ip_net, &p->options[idx+2], 4);
    }

    if (rt_local.dns[0] == 0 && gw_host != 0) rt_local.dns[0] = gw_host;
    rt_local.xid = (uint16_t)xid;

    l3_ipv4_update(l3_id, ip_host, mask_host, gw_host, IPV4_CFG_DHCP, &rt_local);

    l3_ipv4_interface_t* v4u = l3_ipv4_find_by_id(l3_id);
    if (v4u) {
        if (!v4u->routing_table) {
            v4u->routing_table = ipv4_rt_create();
        } else {
            ipv4_rt_clear(v4u->routing_table);
        }
        if (v4u->routing_table) {
            if (ip_host && mask_host) {
                uint32_t net = ip_host & mask_host;
                ipv4_rt_add_in(v4u->routing_table, net, mask_host, 0, 10);
            }
            if (gw_host) {
                ipv4_rt_add_in(v4u->routing_table, 0, 0, gw_host, 11);
            }
        }
    }

    st->t1_left_ms = t1_s * 1000;
    st->t2_left_ms = t2_s * 1000;
    st->lease_left_ms = lease_s * 1000;
}

static void dhcp_send_discover_for(dhcp_if_state_t* st) {
    rng_t rng;
    rng_init_random(&rng);
    uint32_t xid = rng_next32(&rng);
    st->trans_xid = xid;
    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) memcpy(req.mac, st->mac, 6);
    sizedptr pkt = dhcp_build_packet(&req, DHCPDISCOVER, xid, DHCPK_DISCOVER, true);
    uint32_t bcast = 0xFFFFFFFFu;
    net_l4_endpoint dst;
    make_ep(bcast, 67, IP_VER4, &dst);
    socket_sendto_udp_ex(st->sock, 0, &dst, 0, (const void*)pkt.ptr, pkt.size);
    free_sized((void*)pkt.ptr, pkt.size);
}

static void dhcp_send_request_select_for(dhcp_if_state_t* st, const dhcp_request* base) {
    sizedptr pkt = dhcp_build_packet(base, DHCPREQUEST, st->trans_xid, DHCPK_SELECT, true);
    uint32_t dip = 0xFFFFFFFFu;
    net_l4_endpoint dst;
    make_ep(dip, 67, IP_VER4, &dst);
    socket_sendto_udp_ex(st->sock, 0, &dst, 0, (const void*)pkt.ptr, pkt.size);
    free_sized((void*)pkt.ptr, pkt.size);
}

static void dhcp_send_renew_for(dhcp_if_state_t* st) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
    if (!v4) return;
    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) memcpy(req.mac, st->mac, 6);
    uint32_t ip_net = bswap32(v4->ip);
    req.offered_ip = ip_net;
    req.server_ip = st->server_ip_net;
    rng_t rng;
    rng_init_random(&rng);
    st->trans_xid = rng_next32(&rng);
    sizedptr pkt = dhcp_build_packet(&req, DHCPREQUEST, st->trans_xid, DHCPK_RENEW, st->server_ip_net == 0);
    uint32_t dip = st->server_ip_net ? st->server_ip_net : 0xFFFFFFFFu;
    net_l4_endpoint dst;
    make_ep(dip, 67, IP_VER4, &dst);
    socket_sendto_udp_ex(st->sock, 0, &dst, 0, (const void*)pkt.ptr, pkt.size);
    free_sized((void*)pkt.ptr, pkt.size);
}

static void dhcp_send_rebind_for(dhcp_if_state_t* st) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
    if (!v4) return;
    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) memcpy(req.mac, st->mac, 6);
    uint32_t ip_net = bswap32(v4->ip);
    req.offered_ip = ip_net;
    req.server_ip = 0;
    rng_t rng;
    rng_init_random(&rng);
    st->trans_xid = rng_next32(&rng);
    sizedptr pkt = dhcp_build_packet(&req, DHCPREQUEST, st->trans_xid, DHCPK_REBIND, true);
    uint32_t dip = 0xFFFFFFFFu;
    net_l4_endpoint dst;
    make_ep(dip, 67, IP_VER4, &dst);
    socket_sendto_udp_ex(st->sock, 0, &dst, 0, (const void*)pkt.ptr, pkt.size);
    free_sized((void*)pkt.ptr, pkt.size);
}

static void dhcp_send_inform_for(dhcp_if_state_t* st) {
    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(st->l3_id);
    if (!v4 || !v4->ip) return;
    dhcp_request req;
    memset(&req, 0, sizeof(req));
    if (st->mac_ok) memcpy(req.mac, st->mac, 6);
    uint32_t ip_net = bswap32(v4->ip);
    req.offered_ip = ip_net;
    req.server_ip = 0;
    rng_t rng;
    rng_init_random(&rng);
    uint32_t xid = rng_next32(&rng);
    sizedptr pkt = dhcp_build_packet(&req, DHCPINFORM, xid, DHCPK_INFORM, true);
    uint32_t dip = 0xFFFFFFFFu;
    net_l4_endpoint dst;
    make_ep(dip, 67, IP_VER4, &dst);
    socket_sendto_udp_ex(st->sock, 0, &dst, 0, (const void*)pkt.ptr, pkt.size);
    free_sized((void*)pkt.ptr, pkt.size);
}

static void schedule_retry(dhcp_if_state_t* st) {
    st->retry_left_ms = dhcp_next_backoff_ms(st);
}

static void fsm_once_for(dhcp_if_state_t* st) {
    dhcp_state_t old = st->state;
    switch (st->state) {
    case DHCP_S_INIT: {
        if (st->retry_left_ms != 0) break;
        dhcp_send_discover_for(st);
        st->last_xid = st->trans_xid;
        st->state = DHCP_S_SELECTING;
    } break;
    case DHCP_S_SELECTING: {
        dhcp_packet* offer = NULL; sizedptr sp = (sizedptr){0,0};
        if (!udp_wait_for_type_on(st->sock, DHCPOFFER, st->last_xid, st->mac_ok ? st->mac : NULL, &offer, &sp, 5000)) {
            st->state = DHCP_S_INIT;
            schedule_retry(st);
            break;
        }
        dhcp_request req;
        memset(&req, 0, sizeof(req));
        if (st->mac_ok) memcpy(req.mac, st->mac, 6);
        uint16_t idx54 = dhcp_parse_option_bounded(offer, sp.size, 54);
        if (idx54 != UINT16_MAX && offer->options[idx54+1] >= 4) memcpy(&st->server_ip_net, &offer->options[idx54+2], 4);
        memcpy(&req.offered_ip, &offer->yiaddr, 4);
        req.server_ip = st->server_ip_net;
        free_sized((void*)sp.ptr, sp.size);
        dhcp_send_request_select_for(st, &req);
        st->state = DHCP_S_REQUESTING;
        dhcp_reset_backoff(st);
    } break;
    case DHCP_S_REQUESTING: {
        dhcp_packet* resp = NULL; sizedptr sp = (sizedptr){0,0}; uint8_t mtype = 0;
        if (!udp_wait_for_ack_or_nak(st->sock, st->last_xid, st->mac_ok ? st->mac : NULL, &resp, &sp, 5000, &mtype)) {
            st->state = DHCP_S_INIT;
            schedule_retry(st);
        } else {
            if (mtype == DHCPACK) {
                apply_offer_to_l3(st->ifindex, st->l3_id, resp, sp, st->last_xid, st);
                free_sized((void*)sp.ptr, sp.size);
                st->state = DHCP_S_BOUND;
                dhcp_reset_backoff(st);
            } else {
                free_sized((void*)sp.ptr, sp.size);
                l3_ipv4_update(st->l3_id, 0, 0, 0, IPV4_CFG_DHCP, NULL);
                st->t1_left_ms = 0;
                st->t2_left_ms = 0;
                st->lease_left_ms = 0;
                st->server_ip_net = 0;
                st->state = DHCP_S_INIT;
                schedule_retry(st);
            }
        }
    } break;
    case DHCP_S_BOUND: {
        if (g_force_renew) {
            g_force_renew = false;
            dhcp_send_renew_for(st);
            st->last_xid = st->trans_xid;
            st->state = DHCP_S_RENEWING;
        } else if (st->t2_left_ms == 0 && st->lease_left_ms != 0) {
            dhcp_send_rebind_for(st);
            st->last_xid = st->trans_xid;
            st->state = DHCP_S_REBINDING;
        } else if (st->t1_left_ms == 0 && st->lease_left_ms != 0) {
            dhcp_send_renew_for(st);
            st->last_xid = st->trans_xid;
            st->state = DHCP_S_RENEWING;
        }
    } break;
    case DHCP_S_RENEWING: {
        dhcp_packet* p = NULL; sizedptr sp = (sizedptr){0,0}; uint8_t mtype = 0;
        if (udp_wait_for_ack_or_nak(st->sock, st->last_xid, st->mac_ok ? st->mac : NULL, &p, &sp, 2000, &mtype)) {
            if (mtype == DHCPACK) {
                apply_offer_to_l3(st->ifindex, st->l3_id, p, sp, st->last_xid, st);
                free_sized((void*)sp.ptr, sp.size);
                st->state = DHCP_S_BOUND;
                dhcp_reset_backoff(st);
            } else {
                free_sized((void*)sp.ptr, sp.size);
                l3_ipv4_update(st->l3_id, 0, 0, 0, IPV4_CFG_DHCP, NULL);
                st->t1_left_ms = 0;
                st->t2_left_ms = 0;
                st->lease_left_ms = 0;
                st->server_ip_net = 0;
                st->state = DHCP_S_INIT;
                schedule_retry(st);
            }
        } else {
            dhcp_send_rebind_for(st);
            st->last_xid = st->trans_xid;
            st->state = DHCP_S_REBINDING;
        }
    } break;
    case DHCP_S_REBINDING: {
        dhcp_packet* p = NULL; sizedptr sp = (sizedptr){0,0}; uint8_t mtype = 0;
        if (udp_wait_for_ack_or_nak(st->sock, st->last_xid, st->mac_ok ? st->mac : NULL, &p, &sp, 2000, &mtype)) {
            if (mtype == DHCPACK) {
                apply_offer_to_l3(st->ifindex, st->l3_id, p, sp, st->last_xid, st);
                free_sized((void*)sp.ptr, sp.size);
                st->state = DHCP_S_BOUND;
                dhcp_reset_backoff(st);
            } else {
                free_sized((void*)sp.ptr, sp.size);
                l3_ipv4_update(st->l3_id, 0, 0, 0, IPV4_CFG_DHCP, NULL);
                st->t1_left_ms = 0;
                st->t2_left_ms = 0;
                st->lease_left_ms = 0;
                st->server_ip_net = 0;
                st->state = DHCP_S_INIT;
                schedule_retry(st);
            }
        } else {
            l3_ipv4_update(st->l3_id, 0, 0, 0, IPV4_CFG_DHCP, NULL);
            st->t1_left_ms = 0;
            st->t2_left_ms = 0;
            st->lease_left_ms = 0;
            st->server_ip_net = 0;
            st->state = DHCP_S_INIT;
            schedule_retry(st);
        }
    } break;
    }
    if (old != st->state) kprintf("[DHCP] ifx=%i l3=%i state %i -> %i", st->ifindex, st->l3_id, old, st->state);
}

static void tick_timers() {
    for (int i = 0; i < g_if_count; i++) {
        if (g_if[i].state == DHCP_S_BOUND) {
            if (g_if[i].t1_left_ms > 100) g_if[i].t1_left_ms -= 100; else if (g_if[i].t1_left_ms) g_if[i].t1_left_ms = 0;
            if (g_if[i].t2_left_ms > 100) g_if[i].t2_left_ms -= 100; else if (g_if[i].t2_left_ms) g_if[i].t2_left_ms = 0;
            if (g_if[i].lease_left_ms > 100) g_if[i].lease_left_ms -= 100; else if (g_if[i].lease_left_ms) g_if[i].lease_left_ms = 0;
        }
        if (g_if[i].retry_left_ms > 100) g_if[i].retry_left_ms -= 100; else if (g_if[i].retry_left_ms) g_if[i].retry_left_ms = 0;
    }
}

static void maybe_send_inform() {
    for (int i = 0; i < g_if_count; i++) {
        if (!g_if[i].needs_inform) continue;
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(g_if[i].l3_id);
        if (!v4 || v4->mode != IPV4_CFG_STATIC || !v4->ip) { g_if[i].needs_inform = false; continue; }
        dhcp_send_inform_for(&g_if[i]);
        g_if[i].needs_inform = false;
    }
}

int dhcp_daemon_entry(int argc, char* argv[]) {
    (void)argc; (void)argv;
    g_pid_dhcpd = (uint16_t)get_current_proc_pid();
    dhcp_set_pid(g_pid_dhcpd);
    for (;;) {
        ensure_inventory();
        if (g_if_count == 0) { msleep(250); continue; }
        for (int i = 0; i < g_if_count; i++) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(g_if[i].l3_id);
            if (!v4) continue;
            if (v4->mode == IPV4_CFG_DHCP) fsm_once_for(&g_if[i]);
        }
        maybe_send_inform();
        msleep(100);
        tick_timers();
    }
}
