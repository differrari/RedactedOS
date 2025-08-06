#include "ipv4.h"
#include "console/kio.h"
#include "std/memfunctions.h"
#include "networking/network.h"
#include "net/link_layer/arp.h"
#include "net/transport_layer/udp.h"
#include "net/transport_layer/tcp.h"
#include "icmp.h"
#include "std/string.h"
#include "types.h"
#include "ipv4_route.h"

extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);
extern void      sleep(uint64_t ms);

static net_runtime_opts_t g_rt_opts;
net_cfg_t g_net_cfg = {
    .ip = 0,
    .mask = 0,
    .gw = 0,
    .mode = NET_MODE_DHCP,
    .rt = &g_rt_opts
};

void ipv4_cfg_init() {
    memset(&g_rt_opts, 0, sizeof(g_rt_opts));
    g_net_cfg.ip = 0;
    g_net_cfg.mask = 0;
    g_net_cfg.gw = 0;
    g_net_cfg.mode = NET_MODE_DHCP;
    g_net_cfg.rt = &g_rt_opts;
    ipv4_rt_init();
}

void ipv4_set_cfg(const net_cfg_t *src) {
    if (!src) return;
    g_net_cfg.ip = src->ip;
    g_net_cfg.mask = src->mask;
    g_net_cfg.gw = src->gw;
    g_net_cfg.mode = src->mode;
    if (src->rt) {
        g_rt_opts = *src->rt;
    } else {
        memset(&g_rt_opts, 0, sizeof(g_rt_opts));
    }
    if(g_net_cfg.ip != 0){
        uint8_t bmac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        arp_table_put(ipv4_broadcast(g_net_cfg.ip, g_net_cfg.mask), bmac, 0, true);
    }
    g_net_cfg.rt = &g_rt_opts;
    ipv4_rt_init();
    if (g_net_cfg.gw) {
        ipv4_rt_add(0, 0, g_net_cfg.gw);
    }
}

const net_cfg_t* ipv4_get_cfg() {
    return &g_net_cfg;
}

string ipv4_to_string(uint32_t ip) {
    return string_format("%i.%i.%i.%i",
                        (ip>>24)&0xFF,
                        (ip>>16)&0xFF,
                        (ip>>8)&0xFF,
                        ip&0xFF);
}

static uint16_t ipv4_checksum(const void *buf, size_t len) {
    const uint16_t *data = buf;
    uint32_t sum = 0;
    for (; len > 1; len -= 2) {
        sum += *data++;
    }
    if (len) {
        sum += *(const uint8_t*)data;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

void ip_input(uintptr_t ip_ptr,
            uint32_t ip_len,
            const uint8_t src_mac[6])
{
    if (ip_len < sizeof(ipv4_hdr_t)) return;
    ipv4_hdr_t *hdr = (ipv4_hdr_t*)ip_ptr;
    uint8_t version = hdr->version_ihl >> 4;
    uint8_t ihl = hdr->version_ihl & 0x0F;
    if (version != 4 || ihl < 5) return;

    uint32_t header_bytes = ihl * 4;
    if (ip_len < header_bytes) return;

    if (hdr->header_checksum != 0) {
        uint16_t recv_ck = hdr->header_checksum;
        hdr->header_checksum = 0;
        if (ipv4_checksum(hdr, header_bytes) != recv_ck) return;
        hdr->header_checksum = recv_ck;
    }

    uint32_t sip = __builtin_bswap32(hdr->src_ip);
    arp_table_put(sip, src_mac, 60000, false);

    uint32_t dip = __builtin_bswap32(hdr->dst_ip);
    const net_cfg_t *cfg = ipv4_get_cfg(); //TODO manage special ip

    uintptr_t payload_ptr = ip_ptr + header_bytes;
    uint32_t payload_len = __builtin_bswap16(hdr->total_length) - header_bytes;
    switch (hdr->protocol) {
        case 1://icmp
            icmp_input(payload_ptr, payload_len, sip, dip);
            break;
        case 6://tcp
            tcp_input(payload_ptr, payload_len, sip, dip);
            break;
        case 17://udp
            udp_input(payload_ptr, payload_len, sip, dip);
            break;
        default:
            //everything elese
            break;
    }
}

void ipv4_send_segment(uint32_t src_ip,
                    uint32_t dst_ip,
                    uint8_t  proto,
                    sizedptr segment)
{
    uint32_t nh_ip;
    if (!ipv4_rt_lookup(dst_ip, &nh_ip)) {
        const net_cfg_t *cfg = ipv4_get_cfg();
        if (cfg && ((dst_ip & cfg->mask) == (cfg->ip & cfg->mask))) {
            nh_ip = dst_ip;
        } else {
            nh_ip = cfg ? cfg->gw : dst_ip;
        }
    }

    uint8_t dst_mac[6];
    bool ok = arp_resolve(nh_ip, dst_mac, 200);
    if (!ok) {
        memset(dst_mac, 0xFF, sizeof(dst_mac));
    }

    uint32_t total = sizeof(eth_hdr_t)
                + sizeof(ipv4_hdr_t)
                + segment.size;
    uintptr_t buf = (uintptr_t)malloc(total);
    if (!buf) return;

    const net_l2l3_endpoint *local = network_get_local_endpoint();
    uintptr_t ptr = create_eth_packet(buf, local->mac, dst_mac, 0x0800);

    ipv4_hdr_t *ip = (ipv4_hdr_t *)ptr;
    ip->version_ihl = (4 << 4) | (sizeof(*ip)/4);
    ip->dscp_ecn = 0;
    ip->total_length = __builtin_bswap16(sizeof(*ip) + segment.size);
    ip->identification = 0;
    ip->flags_frag_offset = __builtin_bswap16(0x4000);
    ip->ttl = 64;
    ip->protocol = proto;
    ip->src_ip = __builtin_bswap32(src_ip);
    ip->dst_ip = __builtin_bswap32(dst_ip);
    ip->header_checksum = 0;
    ip->header_checksum = ipv4_checksum(ip, sizeof(*ip));

    ptr += sizeof(*ip);

    if (segment.size) {
        memcpy((void*)ptr, (void*)segment.ptr, segment.size);
    }

    eth_send_frame(buf, total);

    free((void*)buf, total);
}
