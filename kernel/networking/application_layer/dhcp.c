#include "dhcp.h"
#include "std/memory.h"
#include "networking/transport_layer/udp.h"
#include "networking/internet_layer/ipv4.h"
#include "networking/link_layer/link_utils.h"
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

uint32_t dhcp_build_packet(const dhcp_request *req, uint8_t msg_type, uint32_t xid, dhcp_req_kind kind, bool broadcast, dhcp_packet *out) {
    if (!req || !out) return 0;

    memset(out, 0, sizeof(*out));
    size_t idx = 0;

    out->op = 1;
    out->htype = 1;
    out->hlen  = MAC_ADDR_LEN;
    out->xid = xid;
    out->flags = broadcast ? bswap16(0x8000) : 0;
    mac_copy(out->chaddr, req->mac);

    if (msg_type == DHCPINFORM) out->ciaddr = req->offered_ip;
    if (msg_type == DHCPREQUEST && (kind == DHCPK_RENEW || kind == DHCPK_REBIND)) out->ciaddr = req->offered_ip;

    out->options[idx++] = DHCP_MAGIC_COOKIE_0;
    out->options[idx++] = DHCP_MAGIC_COOKIE_1;
    out->options[idx++] = DHCP_MAGIC_COOKIE_2;
    out->options[idx++] = DHCP_MAGIC_COOKIE_3;

    out->options[idx++] = 53;
    out->options[idx++] = 1;
    out->options[idx++] = msg_type;

    if (msg_type == DHCPREQUEST && kind == DHCPK_SELECT) {
        out->options[idx++] = 50;
        out->options[idx++] = 4;
        memcpy(&out->options[idx], &req->offered_ip, 4);
        idx += 4;
        if (req->server_ip) {
            out->options[idx++] = 54;
            out->options[idx++] = 4;
            memcpy(&out->options[idx], &req->server_ip, 4);
            idx += 4;
        }
    } else if (msg_type == DHCPDECLINE && kind == DHCPK_DECLINE) {
        out->options[idx++] = 50;
        out->options[idx++] = 4;
        memcpy(&out->options[idx], &req->offered_ip, 4);
        idx += 4;
        out->options[idx++] = 54;
        out->options[idx++] = 4;
        memcpy(&out->options[idx], &req->server_ip, 4);
        idx += 4; 
    }

    if (msg_type != DHCPDECLINE) idx = dhcp_options_write_param_req_list(out->options, idx);

    out->options[idx++] = 255;

    return (uint32_t)(sizeof(*out) - (sizeof(out->options) - idx));
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
    if (max > sizeof(p->options)) max = sizeof(p->options);
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
