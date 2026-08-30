#include "udp.h"
#include "net/checksums.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/internet_layer/ipv6.h"
#include "networking/transport_layer/socket_bind.h"
#include "networking/transport_layer/csocket_udp.h"
#include "std/memory.h"
#include "types.h"
#include "syscalls/syscalls.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"

size_t create_udp_segment(uintptr_t buf, const net_l4_endpoint *src, const net_l4_endpoint *dst, sizedptr payload) {
    if (!buf || !src || !dst || src->ver != dst->ver) return 0;
    if ((src->ver != IP_VER4 && src->ver != IP_VER6) || (payload.size && !payload.ptr)) return 0;
    if (payload.size > UINT16_MAX - sizeof(udp_hdr_t)) return 0;

    udp_hdr_t udp;
    udp.src_port = bswap16(src->port);
    udp.dst_port = bswap16(dst->port);
    uint16_t full_len = (uint16_t)(sizeof(udp) + payload.size);
    udp.length = bswap16(full_len);
    udp.checksum = 0;

    memcpy((void*)buf, &udp, sizeof(udp));
    if (payload.size) memcpy((void *)(buf + sizeof(udp)), (void *)payload.ptr, payload.size);

    uint16_t checksum = 0;
    if (src->ver == IP_VER4) {
        uint32_t src_ip = 0;
        uint32_t dst_ip = 0;
        memcpy(&src_ip, src->ip, 4);
        memcpy(&dst_ip, dst->ip, 4);
        checksum = checksum16_pipv4(src_ip, dst_ip, PROTO_UDP, (const uint8_t *)buf, full_len);
    } else checksum = checksum16_pipv6(src->ip, dst->ip, PROTO_UDP, (const uint8_t *)buf, full_len);

    udp.checksum = checksum ? bswap16(checksum) : UINT16_MAX;
    memcpy((void*)buf, &udp, sizeof(udp));
    return full_len;
}

bool udp_send_segment(const net_l4_endpoint *src, const net_l4_endpoint *dst, sizedptr payload, const ip_tx_opts_t* tx_opts, uint8_t ttl, uint8_t dontfrag) {
    if (!src || !dst || src->ver != dst->ver) return false;
    if ((src->ver != IP_VER4 && src->ver != IP_VER6) || (payload.size && !payload.ptr)) return false;
    if (payload.size > UINT16_MAX - sizeof(udp_hdr_t)) return false;

    uint32_t udp_len = (uint32_t)(sizeof(udp_hdr_t) + payload.size);
    uint32_t headroom = (uint32_t)sizeof(eth_hdr_t) + (uint32_t)(src->ver == IP_VER4 ? sizeof(ipv4_hdr_t) : sizeof(ipv6_hdr_t));
    netpkt_t* pkt = netpkt_alloc(udp_len, headroom, 0);
    if (!pkt) return false;
    void* buf = netpkt_put(pkt, udp_len);
    if (!buf) {
        netpkt_unref(pkt);
        return false;
    }

    if (!create_udp_segment((uintptr_t)buf, src, dst, payload)) {
        netpkt_unref(pkt);
        return false;
    }

    if (src->ver == IP_VER4) {
        uint32_t dst_ip = 0;
        memcpy(&dst_ip, dst->ip, 4);
        return ipv4_send_packet(dst_ip, PROTO_UDP, pkt, tx_opts, ttl, dontfrag);
    }
    return ipv6_send_packet(dst->ip, PROTO_UDP, pkt, tx_opts, ttl, dontfrag);
}

bool udp_strip_header(const netpkt_t* pkt, udp_hdr_t* hdr, uint32_t* payload_off, uint32_t* payload_len) {
    if (!pkt || !hdr || !payload_off || !payload_len) return false;
    uint32_t len = netpkt_len(pkt);
    if (len < sizeof(udp_hdr_t)) return false;
    if (!netpkt_copyout(pkt, 0, hdr, sizeof(*hdr))) return false;
    
    uint16_t total = bswap16(hdr->length);
    if (total < sizeof(udp_hdr_t) || total > len) return false;
    *payload_off = (uint32_t)sizeof(udp_hdr_t);
    *payload_len = total - (uint32_t)sizeof(udp_hdr_t);
    return true;
}

void udp_input(ip_version_t ipver, const void *src_ip_addr, const void *dst_ip_addr, l3_id_t l3_id, netpkt_t* pkt) {
    if (!pkt) return;
    udp_hdr_t hdr;
    uint32_t payload_off = 0;
    uint32_t payload_len = 0;
    if (!udp_strip_header(pkt, &hdr, &payload_off, &payload_len)) {
        netpkt_unref(pkt);
        return;
    }

    if (ipver == IP_VER6 && !hdr.checksum) {
        netpkt_unref(pkt);
        return;
    }
    if (hdr.checksum) {
        if (ipver == IP_VER4) {
            uint32_t src_ip = 0;
            uint32_t dst_ip = 0;
            memcpy(&src_ip, src_ip_addr, sizeof(src_ip));
            memcpy(&dst_ip, dst_ip_addr, sizeof(dst_ip));
            if (checksum16_pipv4(src_ip, dst_ip, PROTO_UDP, (const uint8_t*)netpkt_data(pkt), (uint16_t)(payload_len + sizeof(hdr))) != 0) {
                netpkt_unref(pkt);
                return;
            }
        } else if (ipver == IP_VER6) {
            if (checksum16_pipv6((const uint8_t*)src_ip_addr, (const uint8_t*)dst_ip_addr, PROTO_UDP, (const uint8_t*)netpkt_data(pkt), (uint32_t)(payload_len + sizeof(hdr))) != 0) {
                netpkt_unref(pkt);
                return;
            }
        }
    }

    uint16_t dst_port = bswap16(hdr.dst_port);
    uint16_t src_port = bswap16(hdr.src_port);

    l3_ipv4_interface_t *v4 = NULL;
    l3_ipv6_interface_t *v6 = NULL;

    if (ipver == IP_VER4) v4 = l3_ipv4_find_by_id(l3_id);
    else if (ipver == IP_VER6) v6 = l3_ipv6_find_by_id(l3_id);

    if (!v4 && !v6) {
        netpkt_unref(pkt);
        return;
    }

    netpkt_t* plpkt = netpkt_view(pkt, payload_off, payload_len);
    if (!plpkt) {
        netpkt_unref(pkt);
        return;
    }

    uint8_t ifx = 0;
    if (v4 && v4->l2) ifx = v4->l2->ifindex;
    else if (v6 && v6->l2) ifx = v6->l2->ifindex;

    bool fanout = false;
    if (ipver == IP_VER4) {
        uint32_t dst = 0;
        memcpy(&dst, dst_ip_addr, sizeof(dst));
        fanout = ipv4_is_multicast(dst) || ipv4_is_limited_broadcast(dst) || (v4 && dst == v4->broadcast);
    } else if (ipver == IP_VER6) fanout = ipv6_is_multicast(dst_ip_addr);

    if (!fanout) {
        ksocket_t* socket = socket_bind_lookup(PROTO_UDP, ipver, l3_id, ifx, src_ip_addr, src_port, dst_ip_addr, dst_port);
        if (socket) {
            socket_udp_input(socket, ipver, l3_id, src_ip_addr, dst_ip_addr, plpkt, src_port, dst_port);
            socket_core_put(socket);
        }
    } else {
        uint32_t cursor = 0;
        ksocket_t* socket = NULL;
        while ((socket = socket_bind_udp_next_fanout(ipver, l3_id, ifx, dst_ip_addr, dst_port, &cursor))) {
            socket_udp_input(socket, ipver, l3_id, src_ip_addr, dst_ip_addr, plpkt, src_port, dst_port);
            socket_core_put(socket);
        }
    }

    netpkt_unref(plpkt);
    netpkt_unref(pkt);
}

