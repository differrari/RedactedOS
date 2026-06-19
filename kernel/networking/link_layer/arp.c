#include "arp.h"
#include "eth.h"
#include "std/memory.h"
#include "std/string.h"
#include "networking/network.h"
#include "process/scheduler.h"
#include "console/kio.h"
#include "networking/internet_layer/ipv4.h"
#include "syscalls/syscalls.h"
#include "networking/internet_layer/ipv4_utils.h"

#define ARP_REACHABLE_MS 180000
#define ARP_STALE_MS 60000
#define ARP_RETRANS_MS 1000
#define ARP_MAX_PROBES 3

struct arp_table {
    arp_entry_t entries[ARP_TABLE_MAX];
    uint8_t init;
};

static uint16_t g_arp_pid = 0xFFFF;

static inline arp_table_t* l2_arp(uint8_t ifindex){
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    return l2 ? (arp_table_t*)l2->arp_table : 0;
}

static void arp_entry_clear(arp_entry_t* e) {
    if (!e) return;
    if (e->pending) {
        for (int i = 0; i < e->pending_len; i++) {
            if (e->pending[i]) netpkt_unref(e->pending[i]);
        }
        release(e->pending);
    }
    memset(e, 0, sizeof(*e));
    e->state = ARP_STATE_UNUSED;
}

arp_table_t* arp_table_create(void){
    arp_table_t* t = (arp_table_t*)zalloc(sizeof(arp_table_t));
    if (!t) return 0;
    t->init = 1;
    arp_table_init_static_defaults(t);
    return t;
}

void arp_table_destroy(arp_table_t* t){
    if (!t) return;
    for (int i = 0; i < ARP_TABLE_MAX; i++) arp_entry_clear(&t->entries[i]);
    release(t);
}

void arp_table_init_static_defaults(arp_table_t* t){
    if (!t) return;
    t->entries[0].ip = 0xFFFFFFFFu;
    t->entries[0].mac[0] = 0xFF;
    t->entries[0].mac[1] = 0xFF;
    t->entries[0].mac[2] = 0xFF;
    t->entries[0].mac[3] = 0xFF;
    t->entries[0].mac[4] = 0xFF;
    t->entries[0].mac[5] = 0xFF;
    t->entries[0].ttl_ms = 0;
    t->entries[0].state = ARP_STATE_REACHABLE;
    t->entries[0].static_entry = 1;
}

static int arp_find_slot(arp_table_t* t, uint32_t ip){
    if (!t) return -1;
    for (int i=0;i<ARP_TABLE_MAX;i++) {
        if (t->entries[i].state == ARP_STATE_UNUSED) continue;
        if (t->entries[i].ip == ip) return i;
    }
    return -1;
}

static int arp_find_free(arp_table_t* t){
    if (!t) return -1;
    for (int i=0;i<ARP_TABLE_MAX;i++) if (t->entries[i].state == ARP_STATE_UNUSED) return i;
    return -1;
}

static int arp_find_replacement(arp_table_t* t) {
    int best = -1;
    uint32_t best_ttl = 0xFFFFFFFFu;
    if (!t) return -1;

    for (int i = 0; i < ARP_TABLE_MAX; i++) {
        arp_entry_t* e = &t->entries[i];
        if (e->static_entry) continue;
        if (e->pending_len) continue;
        if (e->state == ARP_STATE_UNUSED) return i;
        if (e->ttl_ms < best_ttl) {
            best_ttl = e->ttl_ms;
            best = i;
        }
    }

    return best;
}

void arp_table_put_for_l2(uint8_t ifindex, uint32_t ip, const uint8_t mac[6], uint32_t ttl_ms, bool is_static){
    arp_table_t* t = l2_arp(ifindex);
    if (!t || !ip || !mac) return;
    int idx = arp_find_slot(t, ip);
    if (idx < 0) idx = arp_find_free(t);
    if (idx < 0) idx = arp_find_replacement(t);
    if (idx < 0) return;

    arp_entry_t* e = &t->entries[idx];
    if (e->state != ARP_STATE_UNUSED && e->ip != ip) arp_entry_clear(e);

    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->ttl_ms = is_static ? 0 : (ttl_ms ? ttl_ms : ARP_REACHABLE_MS);
    e->timer_ms = 0;
    e->state = ARP_STATE_REACHABLE;
    e->probes_sent = 0;
    e->static_entry = is_static ? 1 : 0;

    if (e->pending) {
        for (int i = 0; i < e->pending_len; i++) {
            netpkt_t* pkt = e->pending[i];
            e->pending[i] = 0;
            if (pkt) (void)eth_send_frame_on(ifindex, ETHERTYPE_IPV4, e->mac, pkt);
        }
        release(e->pending);
        e->pending = 0;
        e->pending_len = 0;
        e->pending_bytes = 0;
    }
}

bool arp_table_get_for_l2(uint8_t ifindex, uint32_t ip, uint8_t mac_out[6]){
    arp_table_t* t = l2_arp(ifindex);
    if (!t || !mac_out) return false;

    for (int i=0;i<ARP_TABLE_MAX;i++){
        arp_entry_t* e = &t->entries[i];
        if (e->state == ARP_STATE_UNUSED || e->state == ARP_STATE_INCOMPLETE) continue;
        if (e->ip != ip) continue;
        memcpy(mac_out, e->mac, 6);
        return true;
    }
    return false;
}

bool arp_table_delete_for_l2(uint8_t ifindex, uint32_t ip) {
    arp_table_t* t = l2_arp(ifindex);
    if (!t || !ip) return false;
    int idx = arp_find_slot(t, ip);
    if (idx < 0) return false;
    arp_entry_clear(&t->entries[idx]);
    return true;
}

uint32_t arp_table_dump_for_l2(uint8_t ifindex, arp_entry_t* out, uint32_t out_cap) {
    arp_table_t* t = l2_arp(ifindex);
    if (!t || !out || !out_cap) return 0;
    uint32_t n = 0;
    for (int i = 0; i < ARP_TABLE_MAX && n < out_cap; i++) {
        arp_entry_t* e = &t->entries[i];
        if (e->state == ARP_STATE_UNUSED) continue;
        out[n++] = *e;
    }
    return n;
}

void arp_table_tick_for_l2(uint8_t ifindex, uint32_t ms){
    arp_table_t* t = l2_arp(ifindex);
    if (!t) return;
    for (int i=0;i<ARP_TABLE_MAX;i++){
        arp_entry_t* e = &t->entries[i];
        if (e->state == ARP_STATE_UNUSED || e->static_entry) continue;

        if (e->state == ARP_STATE_INCOMPLETE) {
            if (e->timer_ms > ms) {
                e->timer_ms -= ms;
                continue;
            }

            if (e->probes_sent >= ARP_MAX_PROBES) {
                arp_entry_clear(e);
                continue;
            }

            e->timer_ms = ARP_RETRANS_MS;
            e->probes_sent++;
            arp_send_request_on(ifindex, e->ip);
            continue;
        }

        if (e->ttl_ms > ms) {
            e->ttl_ms -= ms;
            continue;
        }

        if (e->state == ARP_STATE_REACHABLE) {
            e->state = ARP_STATE_STALE;
            e->ttl_ms = ARP_STALE_MS;
            continue;
        }

        arp_entry_clear(e);
    }
}

void arp_tick_all(uint32_t ms){
    for (uint8_t i=1;i<=MAX_L2_INTERFACES;i++){
        l2_interface_t* l2 = l2_interface_find_by_index(i);
        if (!l2) continue;
        if (!l2->arp_table) continue;
        arp_table_tick_for_l2(i, ms);
    }
}

static uint32_t pick_spa_for_l2(uint8_t ifindex, uint32_t target_ip){
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return 0;
    for (int s=0;s<MAX_IPV4_PER_INTERFACE;s++){
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!ipv4_l3_is_ready(v4)) continue;
        if (v4->mask){
            uint32_t a = v4->ip & v4->mask;
            uint32_t b = target_ip & v4->mask;
            if (a == b) return v4->ip;
        }
    }
    for (int s=0;s<MAX_IPV4_PER_INTERFACE;s++){
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!ipv4_l3_is_ready(v4)) continue;
        return v4->ip;
    }
    return 0;
}

bool arp_send_or_queue_on(uint8_t ifindex, uint32_t ip, netpkt_t* pkt) {
    if (!pkt || !netpkt_len(pkt)) {
        if (pkt) netpkt_unref(pkt);
        return false;
    }

    uint8_t mac[6];
    if (ip == 0xFFFFFFFFu){
        memset(mac, 0xFF, 6);
        return eth_send_frame_on(ifindex, ETHERTYPE_IPV4, mac, pkt);
    }
    if (arp_table_get_for_l2(ifindex, ip, mac)) return eth_send_frame_on(ifindex, ETHERTYPE_IPV4, mac, pkt);

    arp_table_t* t = l2_arp(ifindex);
    if (!t || !ip) {
        netpkt_unref(pkt);
        return false;
    }

    int idx = arp_find_slot(t, ip);
    if (idx < 0) idx = arp_find_free(t);
    if (idx < 0) idx = arp_find_replacement(t);
    if (idx < 0) {
        netpkt_unref(pkt);
        return false;
    }

    arp_entry_t* e = &t->entries[idx];
    if (e->state != ARP_STATE_UNUSED && e->ip != ip) arp_entry_clear(e);

    uint32_t len = netpkt_len(pkt);
    if (e->pending_len >= ARP_PENDING_MAX || e->pending_bytes + len > ARP_PENDING_MAX_BYTES) {
        netpkt_unref(pkt);
        return false;
    }

    if (!e->pending) {
        e->pending = (netpkt_t**)zalloc(sizeof(netpkt_t*) * ARP_PENDING_MAX);
        if (!e->pending) {
            netpkt_unref(pkt);
            return false;
        }
    }

    e->ip = ip;
    e->state = ARP_STATE_INCOMPLETE;
    e->ttl_ms = ARP_REACHABLE_MS;
    e->static_entry = 0;
    e->pending[e->pending_len] = pkt; 
    e->pending_len++;
    e->pending_bytes += len;

    if (!e->probes_sent || !e->timer_ms) {
        e->timer_ms = ARP_RETRANS_MS;
        e->probes_sent++;
        arp_send_request_on(ifindex, ip);
    }

    return true;
}

void arp_send_request_on(uint8_t ifindex, uint32_t target_ip){
    const uint8_t* local_mac = network_get_mac(ifindex);
    if (!local_mac) return;
    uint32_t spa = pick_spa_for_l2(ifindex, target_ip);
    uint8_t dst_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    arp_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.htype     = bswap16(1);
    hdr.ptype     = bswap16(ETHERTYPE_IPV4);
    hdr.hlen      = 6;
    hdr.plen      = 4;
    hdr.opcode    = bswap16(ARP_OPCODE_REQUEST);
    memcpy(hdr.sender_mac, local_mac, 6);
    hdr.sender_ip = bswap32(spa);
    hdr.target_ip = bswap32(target_ip);
    netpkt_t* pkt = netpkt_alloc((uint32_t)sizeof(hdr), (uint32_t)sizeof(eth_hdr_t), 0);
    if (!pkt) return;
    void* p = netpkt_put(pkt, (uint32_t)sizeof(hdr));
    if (!p) {
        netpkt_unref(pkt);
        return;
    }
    memcpy(p, &hdr, sizeof(hdr));
    (void)eth_send_frame_on(ifindex, ETHERTYPE_ARP, dst_mac, pkt);
}

static bool l2_has_ip(uint8_t ifindex, uint32_t ip){
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;
    for (int s=0;s<MAX_IPV4_PER_INTERFACE;s++){
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!ipv4_l3_is_ready(v4)) continue;
        if (v4->ip == ip) return true;
    }
    return false;
}

static void arp_send_reply_on(uint8_t ifindex, const arp_hdr_t* in_arp, const uint8_t in_src_mac[6]){
    const uint8_t* local_mac = network_get_mac(ifindex);
    if (!local_mac) return;
    uint32_t spa = pick_spa_for_l2(ifindex, bswap32(in_arp->sender_ip));
    if (!spa) return;
    arp_hdr_t reply = *in_arp;
    memcpy(reply.target_mac, in_arp->sender_mac, 6);
    memcpy(reply.sender_mac, local_mac, 6);
    reply.target_ip = in_arp->sender_ip;
    reply.sender_ip = bswap32(spa);
    reply.opcode    = bswap16(ARP_OPCODE_REPLY);
    netpkt_t* pkt = netpkt_alloc((uint32_t)sizeof(reply), (uint32_t)sizeof(eth_hdr_t), 0);
    if (!pkt) return;
    void* p = netpkt_put(pkt, (uint32_t)sizeof(reply));
    if (!p) {
        netpkt_unref(pkt);
        return;
    }
    memcpy(p, &reply, sizeof(reply));
    (void)eth_send_frame_on(ifindex, ETHERTYPE_ARP, in_src_mac, pkt);
}

void arp_input(uint16_t ifindex, const uint8_t src_mac[6], netpkt_t* pkt) {
    if (!pkt || !src_mac) return;
    if (netpkt_len(pkt) < (uint32_t)sizeof(arp_hdr_t)) return;

    arp_hdr_t hdr;
    if (!netpkt_copyout(pkt, 0, &hdr, sizeof(hdr))) return;
    uint16_t op = bswap16(hdr.opcode);
    uint32_t sender_ip = bswap32(hdr.sender_ip);
    uint32_t target_ip = bswap32(hdr.target_ip);

    bool sender_mac_matches = memcmp(hdr.sender_mac, src_mac, 6) == 0;
    bool sender_is_usable = sender_ip != 0 && sender_ip != 0xFFFFFFFF && !ipv4_is_multicast(sender_ip) && !l2_has_ip((uint8_t)ifindex, sender_ip);
    if (sender_mac_matches && sender_is_usable) arp_table_put_for_l2((uint8_t)ifindex, sender_ip, hdr.sender_mac, ARP_REACHABLE_MS, false);

    if (op == ARP_OPCODE_REQUEST) {
        bool has = l2_has_ip((uint8_t)ifindex, target_ip);

        uint32_t spa_guess = pick_spa_for_l2((uint8_t)ifindex, sender_ip);
        if (has || (spa_guess == target_ip)) {
            arp_send_reply_on((uint8_t)ifindex, &hdr, src_mac);
        }
    }
}


uint16_t arp_get_pid(void){ return g_arp_pid; }

int arp_daemon_entry(int argc, char* argv[]){
    (void)argc; (void)argv;
    g_arp_pid = get_current_proc_pid();
    const uint32_t tick_ms = 1000;
    while (1){
        arp_tick_all(tick_ms);
        msleep(tick_ms);
    }
}
