#include "arp.h"
#include "eth.h"
#include "console/kio.h"
#include "std/memfunctions.h"
#include "net/internet_layer/ipv4.h"
#include "networking/network.h"
#include "process/scheduler.h"
#include "types.h"
#include "std/string.h"
#include "networking/network.h"


#define ARP_OPCODE_REQUEST 1
#define ARP_OPCODE_REPLY 2

extern void      sleep(uint64_t ms);
extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);

static uint16_t g_arp_pid = 0xFFFF;
static arp_entry_t g_arp_table[ARP_TABLE_MAX];
static bool init = false;
#define KP(fmt, ...) \
    do { kprintf(fmt, ##__VA_ARGS__); } while (0)

void arp_set_pid(uint16_t pid) { g_arp_pid = pid; }
uint16_t arp_get_pid() { return g_arp_pid; }

void arp_table_init() {
    KP("[ARP] init");
    memset(g_arp_table, 0, sizeof(g_arp_table));
    init = true;
    arp_table_init_static_defaults();
}

void arp_table_init_static_defaults() {
    uint8_t bmac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    arp_table_put(0xFFFFFFFF, bmac, 0, true);
}

static int arp_table_find_slot(uint32_t ip) {
    for (int i = 0; i < ARP_TABLE_MAX; i++) {
        if (g_arp_table[i].ip == ip) return i;
    }
    return -1;
}

static int arp_table_find_free() {
    for (int i = 0; i < ARP_TABLE_MAX; i++) {
        if (g_arp_table[i].ip == 0) return i;
    }
    return -1;
}

void arp_table_put(uint32_t ip, const uint8_t mac[6], uint32_t ttl_ms, bool is_static) {
    int idx = arp_table_find_slot(ip);
    if (idx < 0) idx = arp_table_find_free();
    if (idx < 0) idx = 0;

    g_arp_table[idx].ip = ip;
    memcpy(g_arp_table[idx].mac, mac, 6);
    g_arp_table[idx].ttl_ms = is_static ? 0 : ttl_ms;
    g_arp_table[idx].static_entry = is_static ? 1 : 0;

    /*KP("[ARP] put %i.%i.%i.%i -> %x:%x:%x:%x:%x:%x static=%i ttl=%i\n",
       (uint64_t)((ip>>24)&0xFF), (uint64_t)((ip>>16)&0xFF),
       (uint64_t)((ip>>8)&0xFF),  (uint64_t)(ip&0xFF),
       (uint64_t)mac[0], (uint64_t)mac[1], (uint64_t)mac[2],
       (uint64_t)mac[3], (uint64_t)mac[4], (uint64_t)mac[5],
       (uint64_t)g_arp_table[idx].static_entry, (uint64_t)ttl_ms);*/
}

bool arp_table_get(uint32_t ip, uint8_t mac_out[6]) {
    int idx = arp_table_find_slot(ip);
    if (idx < 0) return false;
    memcpy(mac_out, g_arp_table[idx].mac, 6);
    return true;
}

void arp_table_tick(uint32_t ms) {
    for (int i = 0; i < ARP_TABLE_MAX; i++) {
        if (g_arp_table[i].ip == 0 || g_arp_table[i].static_entry)
            continue;
        if (g_arp_table[i].ttl_ms <= ms) {
            memset(&g_arp_table[i], 0, sizeof(arp_entry_t));
        } else {
            g_arp_table[i].ttl_ms -= ms;
        }
    }
}

bool arp_resolve(uint32_t ip, uint8_t mac_out[6], uint32_t timeout_ms) {
    
    if (arp_table_get(ip, mac_out)) return true;
    if (ip == 0xFFFFFFFF) {
        memset(mac_out, 0xFF, 6);
        return true;
    }
    arp_send_request(ip);
    
    uint32_t waited = 0;
    const uint32_t POLL_MS = 100;
    while (waited < timeout_ms) {
        arp_table_tick(POLL_MS);
        if (arp_table_get(ip, mac_out)) return true;
        sleep(POLL_MS);
        waited += POLL_MS;
    }
    return false;
}

void arp_send_request(uint32_t target_ip) {
    const net_l2l3_endpoint *ep = network_get_local_endpoint();
    uint8_t dst_mac[6];
    arp_hdr_t hdr;
    uintptr_t buf;
    uint32_t len;

    memset(dst_mac, 0xFF, sizeof(dst_mac));
    memset(hdr.target_mac, 0x00, sizeof(hdr.target_mac));

    hdr.htype     = __builtin_bswap16(1);
    hdr.ptype     = __builtin_bswap16(0x0800);
    hdr.hlen      = 6;
    hdr.plen      = 4;
    hdr.opcode = __builtin_bswap16(1);
    memcpy(hdr.sender_mac, ep->mac, 6);
    hdr.sender_ip = __builtin_bswap32(ep->ip);
    hdr.target_ip = __builtin_bswap32(target_ip);

    len = sizeof(eth_hdr_t) + sizeof(arp_hdr_t);
    buf = (uintptr_t)malloc(len);
    if (!buf) return;

    uintptr_t ptr = create_eth_packet(buf, ep->mac, dst_mac, 0x0806);
    memcpy((void*)ptr, &hdr, sizeof(arp_hdr_t));

    eth_send_frame(buf, len);
    free((void*)buf, len);
}

bool arp_should_handle(const arp_hdr_t *arp, uint32_t my_ip) {
    return __builtin_bswap32(arp->target_ip) == my_ip;
}

void arp_populate_response(net_l2l3_endpoint *ep, const arp_hdr_t *arp) {
    memcpy(ep->mac, arp->sender_mac, 6);
    ep->ip = __builtin_bswap32(arp->sender_ip);
}

bool arp_can_reply() {
    const net_cfg_t *cfg = ipv4_get_cfg();
    return (cfg && cfg->ip != 0 && cfg->mode != NET_MODE_DISABLED);
}

void arp_daemon_entry() {
    while (1){
        const net_cfg_t *cfg = ipv4_get_cfg();
        if(cfg && cfg->ip != 0 && cfg->mode != NET_MODE_DISABLED) break;
        sleep(200);
    }
    arp_table_init();

    while (1) {
        arp_table_tick(1000);
        sleep(1000);
    }
}
static void arp_send_reply(const arp_hdr_t *in_arp,
                           const uint8_t in_src_mac[6],
                           uint32_t frame_len) {
    const net_l2l3_endpoint *ep = network_get_local_endpoint();

    uint32_t len = sizeof(eth_hdr_t) + sizeof(arp_hdr_t);
    uintptr_t buf = (uintptr_t)malloc(len);
    if (!buf) return;

    uintptr_t ptr = create_eth_packet(buf,
                                    ep->mac,
                                    in_src_mac,
                                    0x0806);

    arp_hdr_t reply = *in_arp;
    memcpy(reply.target_mac, in_arp->sender_mac, 6);
    memcpy(reply.sender_mac, ep->mac,          6);
    reply.target_ip = in_arp->sender_ip;
    reply.sender_ip = __builtin_bswap32(ep->ip);
    reply.opcode    = __builtin_bswap16(ARP_OPCODE_REPLY);

    memcpy((void*)ptr, &reply, sizeof(reply));

    eth_send_frame(buf, len);
    free((void*)buf, len);
}


void arp_input(uintptr_t frame_ptr, uint32_t frame_len) {
    if (frame_len < sizeof(eth_hdr_t) + sizeof(arp_hdr_t)) return;

    if(!init) return;

    arp_hdr_t *hdr = (arp_hdr_t*)(frame_ptr + sizeof(eth_hdr_t));
    uint32_t sender_ip = __builtin_bswap32(hdr->sender_ip);

    arp_table_put(sender_ip, hdr->sender_mac, 180000, false);

    const net_l2l3_endpoint *ep = network_get_local_endpoint();
    if (__builtin_bswap16(hdr->opcode) == ARP_OPCODE_REQUEST &&
        arp_should_handle(hdr, ep->ip) &&
        arp_can_reply())
    {
        const arp_hdr_t *hdr = (arp_hdr_t*)(frame_ptr + sizeof(eth_hdr_t));
        const uint8_t *src_mac  = hdr->sender_mac;
        arp_send_reply(hdr, src_mac, frame_len);
    }
}