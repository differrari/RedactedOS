#include "dhcp.h"
#include "std/memfunctions.h"
#include "net/transport_layer/udp.h"
#include "net/internet_layer/ipv4.h"
#include "types.h"
#include "net/transport_layer/csocket_udp.h"

static socket_handle_t g_dhcp_socket = NULL;

extern uintptr_t malloc(uint64_t size);
extern void      free(void *ptr, uint64_t size);
extern void      sleep(uint64_t ms);
sizedptr dhcp_build_packet(const dhcp_request *req,
                                  uint8_t msg_type,
                                  uint32_t xid)
{
    dhcp_packet p;
    memset(&p, 0, sizeof(p));
    size_t idx = 0;

    p.op = 1; p.htype = 1; p.hlen  = 6; p.hops  = 0;
    p.xid = xid; p.secs  = 0;
    p.flags = __builtin_bswap16(0x8000);
    p.ciaddr = 0; p.yiaddr = 0; p.siaddr = 0; p.giaddr = 0;
    memcpy(p.chaddr, req->mac, 6);

    p.options[idx++] = 0x63; p.options[idx++] = 0x82;
    p.options[idx++] = 0x53; p.options[idx++] = 0x63;

    p.options[idx++] = 53; p.options[idx++] = 1;
    p.options[idx++] = msg_type;

    if (msg_type == DHCPREQUEST) {
        p.options[idx++] = 50; p.options[idx++] = 4;
        memcpy(&p.options[idx], &req->offered_ip, 4); idx += 4;
        if (req->server_ip) {
            p.options[idx++] = 54; p.options[idx++] = 4;
            memcpy(&p.options[idx], &req->server_ip, 4); idx += 4;
        }
    }

    p.options[idx++] = 255;

    size_t dhcp_len = sizeof(dhcp_packet) - (sizeof(p.options) - idx);

    uintptr_t buf = malloc(dhcp_len);
    memcpy((void*)buf, &p, dhcp_len);

    return (sizedptr){ .ptr = buf, .size = (uint32_t)dhcp_len };
}

dhcp_packet* dhcp_parse_frame_payload(uintptr_t frame_ptr) {
    return (dhcp_packet*)frame_ptr;
}

uint16_t dhcp_parse_option(const dhcp_packet *p, uint16_t wanted) {
    const uint8_t *opt = p->options;
    size_t i= 4;
    while (i < sizeof(p->options)) {
        uint8_t code = opt[i++];
        if (code == 0) continue;
        if (code == 255) break;
        if (i >= sizeof(p->options)) break;
        uint8_t len = opt[i++];
        if (code == wanted) {
            return (uint16_t)(i - 2);
        }
        i += len;
    }
    return UINT16_MAX; 
}

uint8_t dhcp_option_len(const dhcp_packet *p, uint16_t idx) {
    if (idx == 0 || idx + 1 >= sizeof(p->options)) return 0;
    return p->options[idx+1];
}
