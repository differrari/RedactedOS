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

typedef struct arp_table {
    arp_entry_t entries[ARP_TABLE_MAX];
    uint8_t init;
} arp_table_t;

static uint16_t g_arp_pid = 0xFFFF;

static inline arp_table_t* l2_arp(uint8_t ifindex){
    l2_interface_t* l2 = l2_interface_find_by_index(ifindex);
    return l2 ? (arp_table_t*)l2->arp_table : 0;
}

arp_table_t* arp_table_create(void){
    arp_table_t* t = (arp_table_t*)malloc(sizeof(arp_table_t));
    if (!t) return 0;
    memset(t, 0, sizeof(*t));
    t->init = 1;
    arp_table_init_static_defaults(t);
    return t;
}

void arp_table_destroy(arp_table_t* t){
    if (t) free_sized(t, sizeof(*t));
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
    t->entries[0].static_entry = 1;
}

static int arp_find_slot(arp_table_t* t, uint32_t ip){
    if (!t) return -1;
    for (int i=0;i<ARP_TABLE_MAX;i++) if (t->entries[i].ip == ip) return i;
    return -1;
}

static int arp_find_free(arp_table_t* t){
    if (!t) return -1;
    for (int i=0;i<ARP_TABLE_MAX;i++) if (t->entries[i].ip == 0) return i;
    return -1;
}

void arp_table_put_for_l2(uint8_t ifindex, uint32_t ip, const uint8_t mac[6], uint32_t ttl_ms, bool is_static){
    arp_table_t* t = l2_arp(ifindex);
    if (!t) return;
    int idx = arp_find_slot(t, ip);
    if (idx < 0) idx = arp_find_free(t);
    if (idx < 0) idx = 0;
    t->entries[idx].ip = ip;
    memcpy(t->entries[idx].mac, mac, 6);
    t->entries[idx].ttl_ms = is_static ? 0 : ttl_ms;
    t->entries[idx].static_entry = is_static ? 1 : 0;
}

bool arp_table_get_for_l2(uint8_t ifindex, uint32_t ip, uint8_t mac_out[6]){
    arp_table_t* t = l2_arp(ifindex);
    if (!t) return false;
    for (int i=0;i<ARP_TABLE_MAX;i++){
        if (t->entries[i].ip == ip){
            memcpy(mac_out, t->entries[i].mac, 6);
            return true;
        }
    }
    return false;
}

void arp_table_tick_for_l2(uint8_t ifindex, uint32_t ms){
    arp_table_t* t = l2_arp(ifindex);
    if (!t) return;
    for (int i=0;i<ARP_TABLE_MAX;i++){
        if (t->entries[i].ip == 0 || t->entries[i].static_entry) continue;
        if (t->entries[i].ttl_ms <= ms){
            memset(&t->entries[i], 0, sizeof(arp_entry_t));
        } else {
            t->entries[i].ttl_ms -= ms;
        }
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
        if (!v4) continue;
        if (v4->mode == IPV4_CFG_DISABLED) continue;
        if (v4->ip && v4->mask){
            uint32_t a = v4->ip & v4->mask;
            uint32_t b = target_ip & v4->mask;
            if (a == b) return v4->ip;
        }
    }
    for (int s=0;s<MAX_IPV4_PER_INTERFACE;s++){
        l3_ipv4_interface_t* v4 = l2->l3_v4[s];
        if (!v4) continue;
        if (v4->mode == IPV4_CFG_DISABLED) continue;
        if (v4->ip) return v4->ip;
    }
    return 0;
}

bool arp_resolve_on(uint8_t ifindex, uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms){
    if (ip == 0xFFFFFFFFu){
        memset(mac_out, 0xFF, 6);
        return true;
    }
    if (arp_table_get_for_l2(ifindex, ip, mac_out)) return true;
    arp_send_request_on(ifindex, ip);
    uint32_t waited = 0;
    const uint32_t POLL_MS = 100;
    while (waited < timeout_ms) {
        arp_table_tick_for_l2(ifindex, POLL_MS);
        if (arp_table_get_for_l2(ifindex, ip, mac_out)) return true;
        msleep(POLL_MS);
        waited += POLL_MS;
    }
    return false;
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
        if (!v4) continue;
        if (v4->mode == IPV4_CFG_DISABLED) continue;
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

void arp_input(uint16_t ifindex, netpkt_t* pkt) {
    if (!pkt) return;
    uint32_t frame_len = netpkt_len(pkt);
    uintptr_t frame_ptr = netpkt_data(pkt);
    if (frame_len < (uint32_t)sizeof(eth_hdr_t) + (uint32_t)sizeof(arp_hdr_t)) return;

    const eth_hdr_t* eth = (const eth_hdr_t*)frame_ptr;
    const uint8_t* src_mac = eth->src_mac;
    const arp_hdr_t* hdr = (const arp_hdr_t*)(frame_ptr + sizeof(eth_hdr_t));
    uint16_t op = bswap16(hdr->opcode);
    uint32_t sender_ip = bswap32(hdr->sender_ip);
    uint32_t target_ip = bswap32(hdr->target_ip);

    arp_table_put_for_l2((uint8_t)ifindex, sender_ip, hdr->sender_mac, 180000, false);

    if (op == ARP_OPCODE_REQUEST) {
        char tbuf[16], abuf[16];
        ipv4_to_string(target_ip, tbuf);

        bool has = l2_has_ip((uint8_t)ifindex, target_ip);

        uint32_t spa_guess = pick_spa_for_l2((uint8_t)ifindex, sender_ip);
        ipv4_to_string(spa_guess, abuf);
        if (has || (spa_guess == target_ip)) {
            arp_send_reply_on((uint8_t)ifindex, hdr, src_mac);
        }
    }
}


void arp_set_pid(uint16_t pid){ g_arp_pid = pid; }
uint16_t arp_get_pid(void){ return g_arp_pid; }

int arp_daemon_entry(int argc, char* argv[]){
    (void)argc; (void)argv;
    arp_set_pid(get_current_proc_pid());
    const uint32_t tick_ms = 10000;
    while (1){
        arp_tick_all(tick_ms);
        msleep(tick_ms);
    }
}
