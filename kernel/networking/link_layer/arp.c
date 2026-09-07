#include "arp.h"
#include "eth.h"
#include "link_utils.h"
#include "std/memory.h"
#include "networking/network.h"
#include "networking/interface_manager.h"
#include "process/scheduler.h"
#include "kernel_processes/kprocess_loader.h"
#include "exceptions/irq.h"
#include "networking/internet_layer/ipv4.h"
#include "syscalls/syscalls.h"
#include "networking/internet_layer/ipv4_utils.h"
#define ARP_REACHABLE_MS 180000
#define ARP_STALE_MS 60000
#define ARP_RETRANS_MS 1000
#define ARP_MAX_PROBES 3
#define ARP_DAD_PROBES 3
#define ARP_DAD_WAIT_MS 150
#define ARP_ANNOUNCE_INTERVAL_MS 2000u
#define ARP_DEFEND_INTERVAL_MS 10000u

typedef struct {
    uint32_t ip;
    uint32_t last_conflict_ms;
    uint8_t used;
} arp_defense_t;

struct arp_table {
    arp_entry_t entries[ARP_TABLE_MAX];
    uint32_t dad_ip;
    uint32_t announce_ip;
    uint32_t announce_timer_ms;
    uint8_t dad_conflict;
    uint8_t announce_left;
    arp_defense_t defense[MAX_IPV4_PER_INTERFACE];
};

static volatile uint8_t g_arp_daemon_running;
static volatile uint8_t g_arp_daemon_pending;

static void arp_daemon_kick(void);

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
    arp_table_init_static_defaults(t);
    return t;
}

void arp_table_destroy(arp_table_t* t){
    if (!t) return;
    for (int i = 0; i < (int)N_ARR(t->entries); i++) arp_entry_clear(&t->entries[i]);
    release(t);
}

void arp_table_init_static_defaults(arp_table_t* t){
    if (!t) return;
    t->entries[0].ip = IPV4_LIMITED_BROADCAST;
    mac_set_broadcast(t->entries[0].mac);
    t->entries[0].ttl_ms = 0;
    t->entries[0].state = ARP_STATE_REACHABLE;
    t->entries[0].static_entry = 1;
}

static int arp_find_slot(arp_table_t* t, uint32_t ip){
    if (!t) return -1;
    for (int i = 0; i < (int)N_ARR(t->entries); i++) {
        if (t->entries[i].state == ARP_STATE_UNUSED) continue;
        if (t->entries[i].ip == ip) return i;
    }
    return -1;
}

static int arp_find_free(arp_table_t* t){
    if (!t) return -1;
    for (int i = 0; i < (int)N_ARR(t->entries); i++) if (t->entries[i].state == ARP_STATE_UNUSED) return i;
    return -1;
}

static int arp_find_replacement(arp_table_t* t) {
    int best = -1;
    uint32_t best_ttl = UINT32_MAX;
    if (!t) return -1;

    for (int i = 0; i < (int)N_ARR(t->entries); i++) {
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

void arp_table_put_for_l2(uint8_t ifindex, uint32_t ip, const uint8_t mac[MAC_ADDR_LEN], uint32_t ttl_ms, bool is_static){
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
    if (!t || !ip || !mac) return;
    int idx = arp_find_slot(t, ip);
    if (idx < 0) idx = arp_find_free(t);
    if (idx < 0) idx = arp_find_replacement(t);
    if (idx < 0) return;

    arp_entry_t* e = &t->entries[idx];
    if (e->state != ARP_STATE_UNUSED && e->ip == ip && e->static_entry && !is_static) return;
    if (e->state != ARP_STATE_UNUSED && e->ip != ip) arp_entry_clear(e);

    e->ip = ip;
    mac_copy(e->mac, mac);
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
    if (!is_static) arp_daemon_kick();
}

static void arp_table_learn_for_l2(uint8_t ifindex, uint32_t ip, const uint8_t mac[MAC_ADDR_LEN], bool confirmed, bool allow_create) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
    if (!t || !ip || !mac) return;

    int idx = arp_find_slot(t, ip);
    if (idx < 0 && !allow_create) return;
    if (idx < 0) idx = arp_find_free(t);
    if (idx < 0) idx = arp_find_replacement(t);
    if (idx < 0) return;

    arp_entry_t* e = &t->entries[idx];
    if (e->state != ARP_STATE_UNUSED && e->ip == ip && e->static_entry) return;
    if (e->state != ARP_STATE_UNUSED && e->ip != ip) arp_entry_clear(e);

    e->ip = ip;
    mac_copy(e->mac, mac);
    e->timer_ms = 0;
    e->state = confirmed ? ARP_STATE_REACHABLE : ARP_STATE_STALE;
    e->ttl_ms = confirmed ? ARP_REACHABLE_MS : ARP_STALE_MS;
    e->probes_sent = 0;
    e->static_entry = 0;

    if (e->pending) {
        for (int i = 0; i < e->pending_len; i++) {
            netpkt_t* pkt = e->pending[i];
            e->pending[i] = 0;
            if (pkt) eth_send_frame_on(ifindex, ETHERTYPE_IPV4, e->mac, pkt);
        }
        release(e->pending);
        e->pending = 0;
        e->pending_len = 0;
        e->pending_bytes = 0;
    }
    arp_daemon_kick();
}

bool arp_table_get_for_l2(uint8_t ifindex, uint32_t ip, uint8_t mac_out[MAC_ADDR_LEN]){
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
    if (!t || !mac_out) return false;

    for (int i = 0; i < (int)N_ARR(t->entries); i++){
        arp_entry_t* e = &t->entries[i];
        if (e->state == ARP_STATE_UNUSED || e->state == ARP_STATE_INCOMPLETE) continue;
        if (e->ip != ip) continue;
        mac_copy(mac_out, e->mac);
        return true;
    }
    return false;
}

bool arp_table_delete_for_l2(uint8_t ifindex, uint32_t ip) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
    if (!t || !ip) return false;
    int idx = arp_find_slot(t, ip);
    if (idx < 0) return false;
    arp_entry_clear(&t->entries[idx]);
    return true;
}

uint32_t arp_table_dump_for_l2(uint8_t ifindex, arp_entry_t* out, uint32_t out_cap) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
    if (!t || !out || !out_cap) return 0;
    uint32_t n = 0;
    for (int i = 0; i < (int)N_ARR(t->entries) && n < out_cap; i++) {
        arp_entry_t* e = &t->entries[i];
        if (e->state == ARP_STATE_UNUSED) continue;
        out[n++] = *e;
    }
    return n;
}

static uint32_t pick_spa_for_l2(uint8_t ifindex, uint32_t target_ip){
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return 0;
    for (int s = 0; s < (int)N_ARR(l2->l3_v4); s++) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!ipv4_l3_is_ready(v4)) continue;
        if (v4->mask){
            uint32_t a = v4->ip & v4->mask;
            uint32_t b = target_ip & v4->mask;
            if (a == b)return v4->ip;
        }
    }

    for (int s = 0; s < (int)N_ARR(l2->l3_v4); s++) {
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!ipv4_l3_is_ready(v4)) continue;
        return v4->ip;
    }
    return 0;
}

static void arp_table_tick_for_l2(uint8_t ifindex, uint32_t ms) {
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
    if (!t) return;
    if (t->announce_left) {
        if (t->announce_timer_ms > ms) t->announce_timer_ms -= ms;
        else {
            t->announce_timer_ms = 0;
            if (arp_send_request_on(ifindex, t->announce_ip, t->announce_ip)) {
                t->announce_left--;
                if (t->announce_left) t->announce_timer_ms = ARP_ANNOUNCE_INTERVAL_MS;
            } else {
                t->announce_left = 0;
                t->announce_ip = 0;
            }
            if (!t->announce_left) t->announce_ip = 0;
        }
    }

    for (int i = 0; i < (int)N_ARR(t->entries); i++){
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
            arp_send_request_on(ifindex, pick_spa_for_l2(ifindex, e->ip), e->ip);
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

bool arp_send_or_queue_on(uint8_t ifindex, uint32_t ip, netpkt_t* pkt) {
    if (!pkt || !netpkt_len(pkt)) {
        if (pkt) netpkt_unref(pkt);
        return false;
    }

    uint8_t mac[MAC_ADDR_LEN];
    if (ip == IPV4_LIMITED_BROADCAST) {
        mac_set_broadcast(mac);
        return eth_send_frame_on(ifindex, ETHERTYPE_IPV4, mac, pkt);
    }
    if (arp_table_get_for_l2(ifindex, ip, mac)) return eth_send_frame_on(ifindex, ETHERTYPE_IPV4, mac, pkt);

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
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
        arp_send_request_on(ifindex, pick_spa_for_l2(ifindex, ip), ip);
    }
    arp_daemon_kick();

    return true;
}

bool arp_send_request_on(uint8_t ifindex, uint32_t sender_ip, uint32_t target_ip){
    const uint8_t* local_mac = network_get_mac(ifindex);
    if (!local_mac || !target_ip) return false;
    uint8_t dst_mac[MAC_ADDR_LEN];
    mac_set_broadcast(dst_mac);
    arp_hdr_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.htype     = bswap16(1);
    hdr.ptype     = bswap16(ETHERTYPE_IPV4);
    hdr.hlen      = MAC_ADDR_LEN;
    hdr.plen      = 4;
    hdr.opcode    = bswap16(ARP_OPCODE_REQUEST);
    mac_copy(hdr.sender_mac, local_mac);
    hdr.sender_ip = bswap32(sender_ip);
    hdr.target_ip = bswap32(target_ip);
    netpkt_t* pkt = netpkt_alloc((uint32_t)sizeof(hdr), (uint32_t)sizeof(eth_hdr_t), 0);
    if (!pkt) return false;
    void* p = netpkt_put(pkt, (uint32_t)sizeof(hdr));
    if (!p) {
        netpkt_unref(pkt);
        return false;
    }
    memcpy(p, &hdr, sizeof(hdr));
    return eth_send_frame_on(ifindex, ETHERTYPE_ARP, dst_mac, pkt);
}

bool arp_dad_ipv4_on(uint8_t ifindex, uint32_t ip) {
    uint8_t mac[MAC_ADDR_LEN];
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
    if (!t || !ip || ip == IPV4_LIMITED_BROADCAST || ipv4_is_multicast(ip)) return false;
    if (arp_table_get_for_l2(ifindex, ip, mac)) return false;

    t->dad_ip = ip;
    t->dad_conflict = 0;

    for (int i = 0; i < ARP_DAD_PROBES; i++) {
        if (!arp_send_request_on(ifindex, 0, ip)) {
            t->dad_ip = 0;
            return false;
        }
        uint32_t start = (uint32_t)get_time();
        while ((uint32_t)get_time() - start < ARP_DAD_WAIT_MS) {
            if (t->dad_conflict || arp_table_get_for_l2(ifindex, ip, mac)) {
                t->dad_ip = 0;
                t->dad_conflict = 0;
                return false;
            }
            msleep(10);
        }
    }

    bool ok = !t->dad_conflict && !arp_table_get_for_l2(ifindex, ip, mac);
    t->dad_ip = 0;
    t->dad_conflict = 0;
    if (!ok) return false;

    if (arp_send_request_on(ifindex, ip, ip)) {
        t->announce_ip = ip;
        t->announce_left = 1;
        t->announce_timer_ms = ARP_ANNOUNCE_INTERVAL_MS;
        arp_daemon_kick();
    }
    return true;
}

static bool l2_has_ip(uint8_t ifindex, uint32_t ip){
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    if (!l2) return false;
    for (int s = 0; s < (int)N_ARR(l2->l3_v4); s++){
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!ipv4_l3_is_ready(v4)) continue;
        if (v4->ip == ip) return true;
    }
    return false;
}

static void arp_send_reply_on(uint8_t ifindex, const arp_hdr_t* in_arp, const uint8_t in_src_mac[MAC_ADDR_LEN]){
    const uint8_t* local_mac = network_get_mac(ifindex);
    if (!local_mac) return;
    uint32_t spa = bswap32(in_arp->target_ip);
    if (!l2_has_ip(ifindex, spa)) return;
    arp_hdr_t reply = *in_arp;
    mac_copy(reply.target_mac, in_arp->sender_mac);
    mac_copy(reply.sender_mac, local_mac);
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

void arp_input(uint8_t ifindex, const uint8_t src_mac[MAC_ADDR_LEN], netpkt_t* pkt) {
    if (!pkt || !src_mac) return;
    if (netpkt_len(pkt) < (uint32_t)sizeof(arp_hdr_t)) return;

    arp_hdr_t hdr;
    if (!netpkt_copyout(pkt, 0, &hdr, sizeof(hdr))) return;
    if (bswap16(hdr.htype) != 1) return;
    if (bswap16(hdr.ptype) != ETHERTYPE_IPV4) return;
    if (hdr.hlen != MAC_ADDR_LEN || hdr.plen != 4) return;

    uint16_t op = bswap16(hdr.opcode);
    if (op != ARP_OPCODE_REQUEST && op != ARP_OPCODE_REPLY) return;
    if (!mac_equal(hdr.sender_mac, src_mac) || !mac_is_unicast(hdr.sender_mac)) return;

    uint32_t sender_ip = bswap32(hdr.sender_ip);
    uint32_t target_ip = bswap32(hdr.target_ip);

    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
    if (!t) return;

    const uint8_t* local_mac = network_get_mac(ifindex);
    bool from_us = local_mac && mac_equal(src_mac, local_mac);
    if (!from_us && t->dad_ip && (sender_ip == t->dad_ip || (!sender_ip && target_ip == t->dad_ip))) t->dad_conflict = 1;

    bool target_is_local = l2_has_ip(ifindex, target_ip);
    bool sender_is_local = sender_ip && l2_has_ip(ifindex, sender_ip);
    if (!from_us && sender_is_local) {
        uint32_t now = (uint32_t)get_time();
        int slot = -1;
        int oldest = -1;
        uint32_t oldest_age = 0;

        for (int i = 0; i < MAX_IPV4_PER_INTERFACE; i++) {
            arp_defense_t* d = &t->defense[i];
            if (d->used && d->ip == sender_ip) {
                slot = i;
                break;
            }
            if (!d->used && slot < 0) slot = i;
            if (!d->used) continue;
            uint32_t age = now - d->last_conflict_ms;
            if (age >= oldest_age) {
                oldest_age = age;
                oldest = i;
            }
        }

        if (slot < 0) slot = oldest;
        arp_defense_t* d = &t->defense[slot];
        bool defend = !d->used || d->ip != sender_ip || now - d->last_conflict_ms >= ARP_DEFEND_INTERVAL_MS;
        d->used = 1;
        d->ip = sender_ip;
        d->last_conflict_ms = now;
        if (defend) arp_send_request_on(ifindex, sender_ip, sender_ip);
    }

    bool garp = sender_ip && sender_ip == target_ip;
    bool sender_is_usable = sender_ip != 0 && sender_ip != IPV4_LIMITED_BROADCAST && !ipv4_is_multicast(sender_ip) && !sender_is_local;
    if (sender_is_usable) {
        bool target_mac_is_us = local_mac && mac_equal(hdr.target_mac, local_mac);
        bool confirmed = op == ARP_OPCODE_REPLY && target_is_local && target_mac_is_us && !garp;
        arp_table_learn_for_l2(ifindex, sender_ip, hdr.sender_mac, confirmed, !garp && target_is_local);
    }

    if (op == ARP_OPCODE_REQUEST) {
        if (l2_has_ip(ifindex, target_ip)) arp_send_reply_on(ifindex, &hdr, src_mac);
    }
}


static bool arp_has_timed_work(void) {
    for (uint8_t ifindex = 1; ifindex <= MAX_L2_INTERFACES; ifindex++) {
        l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
        arp_table_t* t = l2 ? (arp_table_t*)l2->arp_table : NULL;
        if (!t) continue;
        if (t->announce_left) return true;

        for (int i = 0; i < (int)N_ARR(t->entries); i++) {
            arp_entry_t* e = &t->entries[i];
            if (e->state != ARP_STATE_UNUSED && !e->static_entry) return true;
        }
    }
    return false;
}

static int arp_daemon_entry(int argc, char* argv[]){
    (void)argc; (void)argv;

    irq_flags_t irq = irq_save_disable();
    g_arp_daemon_pending = 0;
    g_arp_daemon_running = 1;
    irq_restore(irq);

    uint32_t last_ms = (uint32_t)get_time();
    while (arp_has_timed_work()) {
        msleep(1000);
        uint32_t now_ms = (uint32_t)get_time();
        uint32_t elapsed_ms = now_ms - last_ms;
        last_ms = now_ms;
        for (uint8_t ifindex = 1; ifindex <= MAX_L2_INTERFACES; ifindex++) {
            l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
            if (!l2 || !l2->arp_table) continue;
            arp_table_tick_for_l2(ifindex, elapsed_ms);
        }
    }

    irq = irq_save_disable();
    g_arp_daemon_running = 0;
    irq_restore(irq);
    arp_daemon_kick();
    return 0;
}

static void arp_daemon_kick(void) {
    if (!arp_has_timed_work()) return;

    irq_flags_t irq = irq_save_disable();
    if (g_arp_daemon_running || g_arp_daemon_pending) {
        irq_restore(irq);
        return;
    }
    g_arp_daemon_pending = 1;
    irq_restore(irq);

    if (!create_kernel_process("arp_daemon", arp_daemon_entry, 0, 0)) {
        irq = irq_save_disable();
        g_arp_daemon_pending = 0;
        irq_restore(irq);
    }
}
