#include "dhcp.h"
#include "std/memory.h"
#include "networking/transport_layer/udp.h"
#include "networking/internet_layer/ipv4.h"
#include "types.h"
#include "syscalls/syscalls.h"

static size_t dhcp_options_write_param_req_list(uint8_t *opt, size_t idx) {
    static const uint8_t prl[] = { 1, 3, 6, 15, 42, 26, 51, 58, 59, 119 };
    size_t n = sizeof(prl);
    if (idx + 2 + n >= 312) return idx;
    opt[idx++] = 55;
    opt[idx++] = (uint8_t)n;
    for (size_t i = 0; i < n; i++) opt[idx++] = prl[i];
    return idx;
}

sizedptr dhcp_build_packet(const dhcp_request *req, uint8_t msg_type, uint32_t xid, dhcp_req_kind kind, bool broadcast) {

    dhcp_packet p;
    memset(&p, 0, sizeof(p));
    size_t idx = 0;

    p.op = 1;
    p.htype = 1;
    p.hlen  = 6;
    p.hops  = 0;
    p.xid = xid;
    p.secs  = 0;
    p.flags = broadcast ? bswap16(0x8000) : 0;
    p.ciaddr = 0;
    p.yiaddr = 0;
    p.siaddr = 0;
    p.giaddr = 0;
    memcpy(p.chaddr, req->mac, 6);

    if (msg_type == DHCPINFORM) p.ciaddr = req->offered_ip;
    if (msg_type == DHCPREQUEST && (kind == DHCPK_RENEW || kind == DHCPK_REBIND)) p.ciaddr = req->offered_ip;

    p.options[idx++] = DHCP_MAGIC_COOKIE_0;
    p.options[idx++] = DHCP_MAGIC_COOKIE_1;
    p.options[idx++] = DHCP_MAGIC_COOKIE_2;
    p.options[idx++] = DHCP_MAGIC_COOKIE_3;

    p.options[idx++] = 53;
    p.options[idx++] = 1;
    p.options[idx++] = msg_type;

    if (msg_type == DHCPREQUEST && kind == DHCPK_SELECT) {
        p.options[idx++] = 50;
        p.options[idx++] = 4;
        memcpy(&p.options[idx], &req->offered_ip, 4);
        idx += 4;
        if (req->server_ip) {
            p.options[idx++] = 54;
            p.options[idx++] = 4;
            memcpy(&p.options[idx], &req->server_ip, 4);
            idx += 4;
        }
    }

    idx = dhcp_options_write_param_req_list(p.options, idx);

    p.options[idx++] = 255;

    size_t dhcp_len = sizeof(dhcp_packet) - (sizeof(p.options) - idx);
    uintptr_t buf = (uintptr_t)malloc(dhcp_len);

    memcpy((void*)buf, &p, dhcp_len);
    return (sizedptr){ .ptr = buf, .size = (uint32_t)dhcp_len };
}

dhcp_packet* dhcp_parse_frame_payload(uintptr_t frame_ptr) {
    return (dhcp_packet*)frame_ptr;
}

bool dhcp_has_valid_cookie(const dhcp_packet *p) {
    return p->options[0] == DHCP_MAGIC_COOKIE_0 &&
           p->options[1] == DHCP_MAGIC_COOKIE_1 &&
           p->options[2] == DHCP_MAGIC_COOKIE_2 &&
           p->options[3] == DHCP_MAGIC_COOKIE_3;
}

uint16_t dhcp_parse_option_bounded(const dhcp_packet *p, uint32_t payload_len, uint8_t wanted) {
    if (payload_len < sizeof(dhcp_packet) - sizeof(p->options)) return UINT16_MAX;
    uint32_t header_sz = sizeof(dhcp_packet) - sizeof(p->options);
    if (payload_len < header_sz + 4) return UINT16_MAX;
    const uint8_t *opt = p->options;
    uint32_t i= 4;
    uint32_t max = payload_len - header_sz;
    while (i < max) {
        uint8_t code = opt[i++];
        if (code == 0) continue;
        if (code == 255) break;
        if (i >= max) break;
        uint8_t len = opt[i++];
        if ((uint32_t)i + len > max) break;
        if (code == wanted)
            return (uint16_t)(i - 2);
        i += len;
    }
    return UINT16_MAX;
}

uint8_t dhcp_option_len(const dhcp_packet *p, uint16_t idx) {
    size_t opt_size = sizeof(p->options);
    if (idx == 0 || (size_t)idx + 1 >= opt_size) return 0;
    return p->options[idx + 1];
}
