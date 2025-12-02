#include "udp.h"
#include "net/checksums.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/port_manager.h"
#include "std/memory.h"
#include "types.h"
#include "syscalls/syscalls.h"

static inline uint32_t v4_u32_from_arr(const uint8_t ip16[16]) {
    uint32_t v = 0;
    memcpy(&v, ip16, 4);
    return v;
}

size_t create_udp_segment(uintptr_t buf, const net_l4_endpoint *src, const net_l4_endpoint *dst, sizedptr payload) {
    udp_hdr_t *udp = (udp_hdr_t *)buf;
    udp->src_port = bswap16(src->port);
    udp->dst_port = bswap16(dst->port);
    uint16_t full_len = (uint16_t)(sizeof(*udp) + payload.size);
    udp->length = bswap16(full_len);
    udp->checksum = 0;

    memcpy((void *)(buf + sizeof(*udp)), (void *)payload.ptr, payload.size);

    if (src->ver == IP_VER4) {
        uint32_t s = v4_u32_from_arr(src->ip);
        uint32_t d = v4_u32_from_arr(dst->ip);
        uint16_t csum = checksum16_pipv4(s, d, 0x11, (const uint8_t *)udp, full_len);
        udp->checksum = bswap16(csum);
    } else if (src->ver == IP_VER6) {
        //TODO IPV6
        udp->checksum = 0;
    }

    return full_len;
}

void udp_send_segment(const net_l4_endpoint *src, const net_l4_endpoint *dst, sizedptr payload, const ipv4_tx_opts_t* tx_opts) {
    if (src->ver == IP_VER4) {
        uint32_t udp_len = (uint32_t)(sizeof(udp_hdr_t) + payload.size);
        uintptr_t buf = (uintptr_t)malloc(udp_len);
        if (!buf) return;

        size_t written = create_udp_segment(buf, src, dst, payload);
        uint32_t dst_ip = v4_u32_from_arr(dst->ip);

        ipv4_send_packet(dst_ip, 0x11, (sizedptr){ buf, (uint32_t)written }, tx_opts, 0);
        free_sized((void *)buf, udp_len);
    } else if (src->ver == IP_VER6) {
        //TODO IPV6
    }
}

sizedptr udp_strip_header(uintptr_t ptr, uint32_t len) {
    if (len < sizeof(udp_hdr_t)) {
        return (sizedptr){ 0, 0 };
    }
    udp_hdr_t *hdr = (udp_hdr_t *)ptr;
    uint16_t total = bswap16(hdr->length);
    if (total < sizeof(udp_hdr_t) || total > len) {
        return (sizedptr){ 0, 0 };
    }
    return (sizedptr){
        .ptr  = ptr + sizeof(udp_hdr_t),
        .size = total - sizeof(udp_hdr_t)
    };
}

void udp_input(ip_version_t ipver, const void *src_ip_addr, const void *dst_ip_addr, uint8_t l3_id, uintptr_t ptr, uint32_t len) {
    sizedptr pl = udp_strip_header(ptr, len);
    if (!pl.ptr) return;

    udp_hdr_t *hdr = (udp_hdr_t *)ptr;

    if (hdr->checksum) {
        if (ipver == IP_VER4) {
            uint16_t recv = hdr->checksum;
            hdr->checksum = 0;
            uint16_t calc = checksum16_pipv4(
                *(const uint32_t *)src_ip_addr, *(const uint32_t *)dst_ip_addr, 0x11,
                (const uint8_t *)hdr, (uint16_t)(pl.size + sizeof(*hdr))
            );
            hdr->checksum = recv;
            if (calc != bswap16(recv)) return;
        } else if (ipver == IP_VER6) {
            //TODO IPV&
        }
    }

    uint16_t dst_port = bswap16(hdr->dst_port);
    uint16_t src_port = bswap16(hdr->src_port);

    l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(l3_id);
    l3_ipv6_interface_t *v6 = v4 ? NULL : l3_ipv6_find_by_id(l3_id);
    port_manager_t *pm = v4 ? ifmgr_pm_v4(l3_id) : (v6 ? ifmgr_pm_v6(l3_id) : NULL);
    if (!pm) return;

    port_recv_handler_t handler = port_get_handler(pm, PROTO_UDP, dst_port);
    if (handler) {
        uintptr_t copy = (uintptr_t)malloc(pl.size);
        if (!copy) return;
        memcpy((void*)copy, (const void*)pl.ptr, pl.size);

        uint8_t ifx = v4 ? v4->l2->ifindex : (v6 && v6->l2 ? v6->l2->ifindex : 0);
        handler(ifx, ipver, src_ip_addr, dst_ip_addr, copy, pl.size, src_port, dst_port);
    }
}

static inline port_manager_t* pm_for_l3(uint8_t l3_id) {
    if (l3_ipv4_find_by_id(l3_id)) return ifmgr_pm_v4(l3_id);
    if (l3_ipv6_find_by_id(l3_id)) return ifmgr_pm_v6(l3_id);
    return NULL;
}

bool udp_bind_l3(uint8_t l3_id, uint16_t port, uint16_t pid, port_recv_handler_t handler) {
    port_manager_t* pm = pm_for_l3(l3_id);
    if (!pm) return false;
    return port_bind_manual(pm, PROTO_UDP, port, pid, handler);
}

bool udp_unbind_l3(uint8_t l3_id, uint16_t port, uint16_t pid) {
    port_manager_t* pm = pm_for_l3(l3_id);
    if (!pm) return false;
    return port_unbind(pm, PROTO_UDP, port, pid);
}

int udp_alloc_ephemeral_l3(uint8_t l3_id, uint16_t pid, port_recv_handler_t handler) {
    port_manager_t* pm = pm_for_l3(l3_id);
    if (!pm) return -1;
    return port_alloc_ephemeral(pm, PROTO_UDP, pid, handler);
}
