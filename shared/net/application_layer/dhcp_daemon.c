#include "dhcp_daemon.h"

#include "console/kio.h"
#include "std/memfunctions.h"
#include "process/scheduler.h"
#include "math/math.h"
#include "math/rng.h"

#include "networking/network.h"
#include "net/application_layer/dhcp.h"
#include "net/internet_layer/ipv4.h"
#include "net/network_types.h"
#include "net/link_layer/arp.h"

#include "net/transport_layer/csocket_udp.h"

#include "../net.h"

extern void      sleep(uint64_t ms);
extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);

#ifndef SOCK_ROLE_SERVER
#define SOCK_ROLE_SERVER 1
#endif

#define DHCPDISCOVER 1
#define DHCPOFFER 2
#define DHCPREQUEST 3
#define DHCPDECLINE 4
#define DHCPACK 5
#define DHCPNAK 6
#define DHCPRELEASE 7
#define DHCPINFORM 8

typedef enum {
    DHCP_S_INIT = 0,
    DHCP_S_SELECTING,
    DHCP_S_REQUESTING,
    DHCP_S_BOUND,
    DHCP_S_RENEWING,
    DHCP_S_REBINDING
} dhcp_state_t;

#define KP(fmt, ...) \
    do { kprintf(fmt, ##__VA_ARGS__); } while(0)
static dhcp_state_t g_state = DHCP_S_INIT;
static net_l2l3_endpoint g_local_ep = {0};
static volatile bool g_force_renew = false;
static uint32_t g_t1_left_ms = 0;
static uint32_t g_t2_left_ms = 0;
static uint16_t g_pid_dhcpd = 0xFFFF;

static socket_handle_t g_sock = 0;

uint16_t get_dhcp_pid() { return g_pid_dhcpd; }
bool dhcp_is_running() { return g_pid_dhcpd != 0xFFFF; }
void dhcp_set_pid(uint16_t p){ g_pid_dhcpd = p;    }
void dhcp_force_renew() { g_force_renew = true; }

static inline uint32_t rd_be32(const uint8_t* p){
    uint32_t v; memcpy(&v, p, 4); return __builtin_bswap32(v);
}

static void log_state_change(dhcp_state_t old, dhcp_state_t now){
    KP("[DHCP] state %i -> %i", old, now);
}

static void dhcp_apply_offer(dhcp_packet *p, dhcp_request *req, uint32_t xid);

static void dhcp_tx_packet(const dhcp_request *req,
                           uint8_t msg_type,
                           uint32_t xid,
                           uint32_t dst_ip)
{
    sizedptr pkt = dhcp_build_packet(req, msg_type, xid);
    socket_sendto_udp(g_sock, dst_ip, 67, (const void*)pkt.ptr, pkt.size);
    free((void*)pkt.ptr, pkt.size);
}

static void dhcp_send_discover(uint32_t xid){
    KP("[DHCP] discover xid=%i", xid);
    dhcp_request req = {0};
    memcpy(req.mac, g_local_ep.mac, 6);
    dhcp_tx_packet(&req, DHCPDISCOVER, xid, 0xFFFFFFFFu);
}

static void dhcp_send_request(const dhcp_request *req,
                            uint32_t xid,
                            bool broadcast)
{
    uint32_t dst = broadcast ? 0xFFFFFFFFu : __builtin_bswap32(req->server_ip);
    //KP("[DHCP] request xid=%i dst=%x\n", (uint64_t)xid, (uint64_t)dst);
    dhcp_tx_packet(req, DHCPREQUEST, xid, dst);
}

static void dhcp_send_renew(uint32_t xid) {
    const net_cfg_t *cfg = ipv4_get_cfg();
    dhcp_request req = {0};
    memcpy(req.mac, g_local_ep.mac, 6);
    req.offered_ip = __builtin_bswap32(cfg->ip);
    req.server_ip  = cfg->rt ? cfg->rt->server_ip : 0;
    uint32_t dst = req.server_ip ? __builtin_bswap32(req.server_ip) : 0xFFFFFFFFu;
    KP("[DHCP] renew xid=%i dst=%x", xid, dst);
    dhcp_tx_packet(&req, DHCPREQUEST, xid, dst);
}

static void dhcp_send_rebind(uint32_t xid) {
    const net_cfg_t *cfg = ipv4_get_cfg();
    dhcp_request req = {0};
    memcpy(req.mac, g_local_ep.mac, 6);
    req.offered_ip = __builtin_bswap32(cfg->ip);
    req.server_ip  = 0;
    KP("[DHCP] rebind xid=%i", xid);
    dhcp_tx_packet(&req, DHCPREQUEST, xid, 0xFFFFFFFFu);
}

static bool dhcp_wait_for_type(uint8_t wanted,
                            dhcp_packet **outp,
                            sizedptr *outsp,
                            uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while(waited < timeout_ms){
        uint8_t buf[1024];
        uint32_t sip; uint16_t sport;
        int64_t r = socket_recvfrom_udp(g_sock, buf, sizeof(buf), &sip, &sport);
        if(r > 0){
            dhcp_packet *p = (dhcp_packet*)buf;
            uint16_t idx= dhcp_parse_option(p, 53);
            if (idx != UINT16_MAX && p->options[idx + 2] == wanted){
                uintptr_t copy = malloc((uint32_t)r);
                memcpy((void*)copy, buf, (size_t)r);
                if (outp) *outp= (dhcp_packet*)copy;
                if (outsp) *outsp = (sizedptr){ copy, (uint32_t)r };
                return true;
            }
        } else {
            sleep(50);
            waited += 50;
        }
    }
    KP("[DHCP] wait timeout type=%i", wanted);
    return false;
}

static void dhcp_fsm_once()
{
    //TODO: use a syscall for the rng
    rng_t rng;
    rng_init_random(&rng);
    uint32_t xid_seed = rng_next32(&rng);
    dhcp_state_t old = g_state;

    switch (g_state) {

    case DHCP_S_INIT: {
        const net_l2l3_endpoint *le = network_get_local_endpoint();
        memcpy(g_local_ep.mac, le->mac, 6);
        g_local_ep.ip = 0;
        xid_seed += 0x1111;
        dhcp_send_discover(xid_seed);
        g_state = DHCP_S_SELECTING;
    } break;

    case DHCP_S_SELECTING: {
        dhcp_packet *offer = NULL; sizedptr sp = {0};
        if (!dhcp_wait_for_type(DHCPOFFER, &offer, &sp, 5000)) {
            g_state = DHCP_S_INIT;
            break;
        }
        dhcp_request req = {0};
        memcpy(req.mac, g_local_ep.mac, 6);
        dhcp_apply_offer(offer, &req, xid_seed);
        free((void*)sp.ptr, sp.size);

        xid_seed += 0x0101;
        dhcp_send_request(&req, xid_seed, true);
        g_state = DHCP_S_REQUESTING;
    } break;

    case DHCP_S_REQUESTING: {
        dhcp_packet *ack = NULL; sizedptr sp = {0};
        if (!dhcp_wait_for_type(DHCPACK, &ack, &sp, 5000)) {
            g_state = DHCP_S_INIT;
        } else {
            dhcp_request dummy = {0};
            memcpy(dummy.mac, g_local_ep.mac, 6);
            dhcp_apply_offer(ack, &dummy, xid_seed);
            free((void*)sp.ptr, sp.size);
            g_state = DHCP_S_BOUND;
        }
    } break;

    case DHCP_S_BOUND: {
        if (g_force_renew) {
            g_force_renew = false;
            xid_seed += 0x2222;
            dhcp_send_renew(xid_seed);
            g_state = DHCP_S_RENEWING;

        } else if (g_t2_left_ms == 0) {
            xid_seed += 0x3333;
            dhcp_send_rebind(xid_seed);
            g_state = DHCP_S_REBINDING;

        } else if (g_t1_left_ms == 0) {
            xid_seed += 0x2222;
            dhcp_send_renew(xid_seed);
            g_state = DHCP_S_RENEWING;
        }
    } break;

    case DHCP_S_RENEWING: {
        dhcp_packet *p = NULL; sizedptr sp = {0};
        if (dhcp_wait_for_type(DHCPACK, &p, &sp, 2000)) {
            dhcp_request dummy = {0};
            memcpy(dummy.mac, g_local_ep.mac, 6);
            dhcp_apply_offer(p, &dummy, xid_seed);
            free((void*)sp.ptr, sp.size);
            g_state = DHCP_S_BOUND;
        } else {
            xid_seed += 0x3333;
            dhcp_send_rebind(xid_seed);
            g_state = DHCP_S_REBINDING;
        }
    } break;

    case DHCP_S_REBINDING: {
        dhcp_packet *p = NULL; sizedptr sp = {0};
        if (dhcp_wait_for_type(DHCPACK, &p, &sp, 2000)) {
            dhcp_request dummy = {0};
            memcpy(dummy.mac, g_local_ep.mac, 6);
            dhcp_apply_offer(p, &dummy, xid_seed);
            free((void*)sp.ptr, sp.size);
            g_state = DHCP_S_BOUND;
        } else {
            net_cfg_t g_net_cfg;
            g_net_cfg.ip = 0;
            g_net_cfg.mode = NET_MODE_DISABLED;
            ipv4_set_cfg(&g_net_cfg);
            g_state = DHCP_S_INIT;
        }
    } break;
    }

    if (old != g_state) log_state_change(old, g_state);
}

void dhcp_daemon_entry(){
    KP("[DHCP] daemon start pid=%i", get_current_proc_pid());
    g_pid_dhcpd = (uint16_t)get_current_proc_pid();
    g_sock = udp_socket_create(SOCK_ROLE_SERVER, g_pid_dhcpd);
    if(socket_bind_udp(g_sock, 68) != 0){
        KP("[DHCP] bind failed\n");
        return;
    }

    for(;;){
        dhcp_fsm_once();
        sleep(100);

        if(g_state == DHCP_S_BOUND){
            if(g_t1_left_ms > 100) g_t1_left_ms -= 100; else g_t1_left_ms = 0;
            if(g_t2_left_ms > 100) g_t2_left_ms -= 100; else g_t2_left_ms = 0;
        }
    }
}

static void dhcp_apply_offer(dhcp_packet *p, dhcp_request *req, uint32_t xid) {
    const net_cfg_t *current = ipv4_get_cfg();
    net_cfg_t cfg_local = *current;
    static net_runtime_opts_t rt_static;
    memset(&rt_static, 0, sizeof(rt_static));
    cfg_local.rt = &rt_static;
    cfg_local.rt->xid = (uint16_t)xid;
    cfg_local.mode = NET_MODE_DHCP;

    uint32_t yi_net = p->yiaddr;
    cfg_local.ip = __builtin_bswap32(yi_net);
    req->offered_ip = yi_net;

    uint16_t idx;
    uint8_t len;
    
    idx = dhcp_parse_option(p, 1);
    if (idx != UINT16_MAX && (len = p->options[idx+1]) >= 4) {
        uint32_t mask_net;
        memcpy(&mask_net, &p->options[idx+2], 4);
        cfg_local.mask = __builtin_bswap32(mask_net);
    }

    idx = dhcp_parse_option(p, 3);
    if (idx != UINT16_MAX && (len = p->options[idx+1]) >= 4) {
        uint32_t gw_net;
        memcpy(&gw_net, &p->options[idx+2], 4);
        cfg_local.gw = __builtin_bswap32(gw_net);
    }

    idx = dhcp_parse_option(p, 6);
    if (idx != UINT16_MAX) {
        len = p->options[idx+1];
        for (int i = 0; i < 2 && (i*4 + 4) <= len; ++i) {
            uint32_t dns_net;
            memcpy(&dns_net, &p->options[idx+2 + i*4], 4);
            cfg_local.rt->dns[i] = __builtin_bswap32(dns_net);
        }
    }

    idx = dhcp_parse_option(p, 42);
    if (idx != UINT16_MAX) {
        len = p->options[idx+1];
        for (int i = 0; i < 2 && (i*4 + 4) <= len; ++i) {
            uint32_t ntp_net;
            memcpy(&ntp_net, &p->options[idx+2 + i*4], 4);
            cfg_local.rt->ntp[i] = __builtin_bswap32(ntp_net);
        }
    }

    idx = dhcp_parse_option(p, 26);
    if (idx != UINT16_MAX && p->options[idx+1] == 2) {
        uint16_t mtu_net;
        memcpy(&mtu_net, &p->options[idx+2], 2);
        cfg_local.rt->mtu = __builtin_bswap16(mtu_net);
    }

    idx = dhcp_parse_option(p, 51);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t lease_net;
        memcpy(&lease_net, &p->options[idx+2], 4);
        cfg_local.rt->lease = __builtin_bswap32(lease_net);
    }

    idx = dhcp_parse_option(p, 58);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t t1_net;
        memcpy(&t1_net, &p->options[idx+2], 4);
        cfg_local.rt->t1 = __builtin_bswap32(t1_net);
    } else {
        cfg_local.rt->t1 = cfg_local.rt->lease / 2;
    }
    idx = dhcp_parse_option(p, 59);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t t2_net;
        memcpy(&t2_net, &p->options[idx+2], 4);
        cfg_local.rt->t2 = __builtin_bswap32(t2_net);
    } else {
        cfg_local.rt->t2 = cfg_local.rt->t1 * 2;
    }

    idx = dhcp_parse_option(p, 54);
    if (idx != UINT16_MAX && p->options[idx+1] >= 4) {
        uint32_t srv_net;
        memcpy(&srv_net, &p->options[idx+2], 4);
        cfg_local.rt->server_ip = __builtin_bswap32(srv_net);
        req->server_ip = srv_net;
    }
    uint32_t bcast = ipv4_broadcast(cfg_local.ip, cfg_local.mask);
    static const uint8_t bmac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    arp_table_put(bcast, bmac, 0, true);

    ipv4_set_cfg(&cfg_local);

    kprintf("Local IP: %i.%i.%i.%i",FORMAT_IP(cfg_local.ip));

    g_t1_left_ms = cfg_local.rt->t1 * 1000;
    g_t2_left_ms = cfg_local.rt->t2 * 1000;
}
