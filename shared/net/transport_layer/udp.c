#include "udp.h"
#include "net/checksums.h"
#include "net/internet_layer/ipv4.h"
#include "networking/port_manager.h"        
#include "std/memfunctions.h"
#include "types.h"

extern void sleep(uint64_t ms);
extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);

static inline uint16_t bswap16(uint16_t v) { return __builtin_bswap16(v); }

bool udp_bind(uint16_t port,
            uint16_t pid,
            port_recv_handler_t handler)
{
    return port_bind_manual(PROTO_UDP, port, pid, handler);
}

int udp_alloc_ephemeral(uint16_t pid,
                        port_recv_handler_t handler)
{
    return port_alloc_ephemeral(PROTO_UDP, pid, handler);
}

bool udp_unbind(uint16_t port,
                uint16_t pid)
{
    return port_unbind(PROTO_UDP, port, pid);
}


size_t create_udp_segment(uintptr_t buf,
                        const net_l4_endpoint *src,
                        const net_l4_endpoint *dst,
                        sizedptr payload)
{
    udp_hdr_t *udp = (udp_hdr_t*)buf;
    udp->src_port = bswap16(src->port);
    udp->dst_port = bswap16(dst->port);
    uint16_t full_len = sizeof(*udp) + payload.size;
    udp->length= bswap16(full_len);
    udp->checksum = 0;

    memcpy((void*)(buf + sizeof(*udp)), (void*)payload.ptr, payload.size);

    uint16_t csum = checksum16_pipv4( src->ip, dst->ip, 0x11, (const uint8_t*)udp, full_len);
    udp->checksum = bswap16(csum);
    return full_len;
}

void udp_send_segment(const net_l4_endpoint *src,
                    const net_l4_endpoint *dst,
                    sizedptr payload)
{
    uint32_t udp_max = sizeof(udp_hdr_t) + payload.size;
    uint32_t ip_max = sizeof(ipv4_hdr_t) + udp_max;
    uint32_t eth_total = sizeof(eth_hdr_t)   + ip_max;

    uintptr_t buf = (uintptr_t)malloc(eth_total);
    if (!buf) return;

    uintptr_t udp_buf = buf + sizeof(eth_hdr_t) + sizeof(ipv4_hdr_t);
    size_t udp_len = create_udp_segment(udp_buf, src, dst, payload);

    ipv4_send_segment(src->ip, dst->ip, 0x11,(sizedptr){ udp_buf, (uint32_t)udp_len });

    free((void*)buf, eth_total);
}

sizedptr udp_strip_header(uintptr_t ptr, uint32_t len) {
    if (len < sizeof(udp_hdr_t)) {
        return (sizedptr){0,0};
    }
    udp_hdr_t *hdr = (udp_hdr_t*)ptr;
    uint16_t total = bswap16(hdr->length);
    if (total < sizeof(udp_hdr_t) || total > len) {
        return (sizedptr){0,0};
    }
    return (sizedptr){
        .ptr  = ptr + sizeof(udp_hdr_t),
        .size = total - sizeof(udp_hdr_t)
    };
}

void udp_input(uintptr_t ptr,
            uint32_t len,
            uint32_t src_ip,
            uint32_t dst_ip)
{
    sizedptr pl = udp_strip_header(ptr, len);
    if (!pl.ptr) return;

    udp_hdr_t *hdr = (udp_hdr_t*)ptr;
    if (hdr->checksum) {
        uint16_t recv = hdr->checksum;
        hdr->checksum = 0;
        uint16_t calc = checksum16_pipv4(
            src_ip, dst_ip, 0x11,
            (const uint8_t*)hdr,
            pl.size + sizeof(*hdr)
        );
        hdr->checksum = recv;
        if (calc != bswap16(recv)) return;
    }

    uint16_t dst_port = bswap16(hdr->dst_port);
    uint16_t src_port = bswap16(hdr->src_port);
    port_recv_handler_t handler = port_get_handler(PROTO_UDP, dst_port);
    if (handler) {
        handler(pl.ptr, pl.size, src_ip, src_port, dst_port);
    }
}