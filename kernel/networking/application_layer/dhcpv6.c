#include "networking/application_layer/dhcpv6.h"

#include "std/memory.h"
#include "std/string.h"

static void opt_append(uint8_t*b, uint32_t cap, uint32_t*off, uint16_t code, const void*data, uint16_t len){
    if (!b || !off) return;
    if (*off + 4u + len > cap) return;

    uint16_t c = bswap16(code);
    uint16_t l = bswap16(len);

    memcpy(b + *off + 0, &c, 2);
    memcpy(b + *off + 2, &l, 2);

    if (len && data) memcpy(b + *off + 4, data, len);

    *off += 4u + len;
}

uint32_t dhcpv6_make_xid24(uint32_t r32){
    uint32_t x = r32 & 0x00FFFFFFu;
    if (!x) x = 1;
    return x;
}

uint32_t dhcpv6_iaid_from_mac(const uint8_t mac[6]){
    if (!mac) return 0;
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
}

bool dhcpv6_build_message(uint8_t*out, uint32_t out_cap, uint32_t*out_len, const net_runtime_opts_v6_t*rt, const uint8_t mac[6], uint8_t msg_type, dhcpv6_req_kind kind, uint32_t xid24, bool want_address) {
    if (!out || !out_len) return false;
    if (out_cap < 4) return false;

    xid24 &= 0x00FFFFFFu;
    if (!xid24) xid24 = 1;

    out[0] = msg_type;
    out[1] = (uint8_t)((xid24 >> 16) & 0xFF);
    out[2] = (uint8_t)((xid24 >> 8) & 0xFF);
    out[3] = (uint8_t)(xid24 & 0xFF);

    uint32_t off = 4;

    uint8_t duid[10];
    uint16_t duid_type = bswap16(3);
    uint16_t hw_type = bswap16(1);

    memcpy(duid + 0, &duid_type, 2);
    memcpy(duid + 2, &hw_type, 2);

    if (mac) memcpy(duid + 4, mac, 6);
    else memset(duid + 4, 0, 6);

    opt_append(out, out_cap, &off, DHCPV6_OPT_CLIENTID, duid, 10);

    if (rt && rt->server_id_len && rt->server_id_len <= DHCPV6_MAX_SERVER_ID) opt_append(out, out_cap, &off, DHCPV6_OPT_SERVERID, rt->server_id, rt->server_id_len);

    uint8_t elapsed[2] = {0, 0};
    opt_append(out, out_cap, &off, DHCPV6_OPT_ELAPSED, elapsed, 2);

    if (msg_type == DHCPV6_MSG_SOLICIT || msg_type == DHCPV6_MSG_REQUEST || msg_type == DHCPV6_MSG_RENEW ||
        msg_type == DHCPV6_MSG_REBIND || msg_type == DHCPV6_MSG_CONFIRM || msg_type == DHCPV6_MSG_RELEASE ||
        msg_type == DHCPV6_MSG_INFORMATION_REQUEST || msg_type == DHCPV6_MSG_DECLINE){

        uint16_t oro[2];
        uint16_t n = 0;

        oro[n++] = bswap16(DHCPV6_OPT_DNS_SERVERS);
        oro[n++] = bswap16(DHCPV6_OPT_NTP_SERVER);

        opt_append(out, out_cap, &off, DHCPV6_OPT_ORO, oro, (uint16_t)(n * 2));
    }

    if (want_address) {
        uint32_t iaid = rt ? rt->iaid : 0;
        uint32_t iaid_pd = iaid ? (iaid ^ 0xA5A5A5A5u) : 0;

        if (kind == DHCPV6K_SELECT){
            uint8_t iana[12];

            uint32_t iaid_net = bswap32(iaid);
            uint32_t t1_net = 0;
            uint32_t t2_net = 0;

            memcpy(iana + 0, &iaid_net, 4);
            memcpy(iana + 4, &t1_net, 4);
            memcpy(iana + 8, &t2_net, 4);

            opt_append(out, out_cap, &off, DHCPV6_OPT_IA_NA, iana, 12);

            if (rt && rt->pd_prefix_len){
                uint8_t iapd[12];

                uint32_t pd_iaid_net = bswap32(iaid_pd);

                memcpy(iapd + 0, &pd_iaid_net, 4);
                memcpy(iapd + 4, &t1_net, 4);
                memcpy(iapd + 8, &t2_net, 4);

                opt_append(out, out_cap, &off, DHCPV6_OPT_IA_PD, iapd, 12);
            }
        } else {
            if (rt && rt->lease){
                uint8_t payload[40];
                uint8_t addr[16];

                memset(addr, 0, 16);

                uint32_t iaid_net = bswap32(iaid);
                uint32_t t1_net = 0;
                uint32_t t2_net = 0;

                memcpy(payload + 0, &iaid_net, 4);
                memcpy(payload + 4, &t1_net, 4);
                memcpy(payload + 8, &t2_net, 4);

                uint16_t code_net = bswap16(DHCPV6_OPT_IAADDR);
                uint16_t len_net = bswap16(24);

                memcpy(payload + 12, &code_net, 2);
                memcpy(payload + 14, &len_net, 2);
                memcpy(payload + 16, addr, 16);

                uint32_t pref_net = bswap32(rt->t1);
                uint32_t valid_net = bswap32(rt->lease);

                memcpy(payload + 32, &pref_net, 4);
                memcpy(payload + 36, &valid_net, 4);

                opt_append(out, out_cap, &off, DHCPV6_OPT_IA_NA, payload, 40);
            } else {
                uint8_t iana[12];

                uint32_t iaid_net = bswap32(iaid);
                uint32_t t1_net = 0;
                uint32_t t2_net = 0;

                memcpy(iana + 0, &iaid_net, 4);
                memcpy(iana + 4, &t1_net, 4);
                memcpy(iana + 8, &t2_net, 4);

                opt_append(out, out_cap, &off, DHCPV6_OPT_IA_NA, iana, 12);
            }

            if (rt && rt->pd_prefix_len){
                uint8_t payload[41];

                uint32_t iaid_net = bswap32(iaid_pd);
                uint32_t t1_net = 0;
                uint32_t t2_net = 0;

                memcpy(payload + 0, &iaid_net, 4);
                memcpy(payload + 4, &t1_net, 4);
                memcpy(payload + 8, &t2_net, 4);

                uint16_t code_net = bswap16(DHCPV6_OPT_IAPREFIX);
                uint16_t len_net = bswap16(25);

                memcpy(payload + 12, &code_net, 2);
                memcpy(payload + 14, &len_net, 2);

                uint32_t pref_net = bswap32(rt->pd_preferred_lft);
                uint32_t valid_net = bswap32(rt->pd_valid_lft);

                memcpy(payload + 16, &pref_net, 4);
                memcpy(payload + 20, &valid_net, 4);

                payload[24] = rt->pd_prefix_len;
                memcpy(payload + 25, rt->pd_prefix, 16);

                opt_append(out, out_cap, &off, DHCPV6_OPT_IA_PD, payload, 41);
            }
        }
    }
    *out_len = off;
    return true;
}

static bool parse_opts(const uint8_t*opt, uint32_t opt_len, uint32_t expect_iaid, dhcpv6_parsed_t*out){
    uint32_t off = 0;
    bool got_addr = false;

    while (off + 4 <= opt_len) {
        uint16_t code_net;
        uint16_t len_net;

        memcpy(&code_net, opt + off + 0, 2);
        memcpy(&len_net, opt + off + 2, 2);

        uint16_t code = bswap16(code_net);
        uint16_t len = bswap16(len_net);

        off += 4;
        if (off + len >opt_len) break;

        if (code == DHCPV6_OPT_SERVERID && len && len <= DHCPV6_MAX_SERVER_ID){
            memcpy(out->server_id, opt + off, len);
            out->server_id_len = len;
            out->has_server_id = true;
        }

        if (code == DHCPV6_OPT_DNS_SERVERS && len >= 16){
            int n = (int)(len / 16);
            if (n > 2) n = 2;

            for (int i = 0; i < n; i++)
                memcpy(out->dns[i], opt + off + (uint32_t)i * 16u, 16);

            out->has_dns = true;
        }

        if (code == DHCPV6_OPT_NTP_SERVER && len >= 16){
            int n = (int)(len / 16);
            if (n > 2) n = 2;

            for (int i = 0; i < n; i++)
                memcpy(out->ntp[i], opt + off + (uint32_t)i * 16u, 16);

            out->has_ntp = true;
        }

        if (code == DHCPV6_OPT_IA_NA && len >= 12){
            uint32_t iaid_net;
            uint32_t t1_net;
            uint32_t t2_net;

            memcpy(&iaid_net, opt + off + 0, 4);
            memcpy(&t1_net, opt + off + 4, 4);
            memcpy(&t2_net, opt + off + 8, 4);

            uint32_t iaid = bswap32(iaid_net);
            if (expect_iaid && iaid != expect_iaid){ off += len; continue; }

            out->t1 = bswap32(t1_net);
            out->t2 = bswap32(t2_net);

            uint32_t sub = off + 12;
            uint32_t end = off + len;

            while (sub + 4 <= end){
                uint16_t sc_net;
                uint16_t sl_net;

                memcpy(&sc_net, opt + sub + 0, 2);
                memcpy(&sl_net, opt + sub + 2, 2);

                uint16_t sc = bswap16(sc_net);
                uint16_t sl = bswap16(sl_net);

                sub += 4;
                if (sub + sl > end) break;

                if (sc == DHCPV6_OPT_IAADDR && sl >= 24){
                    memcpy(out->addr, opt + sub + 0, 16);

                    uint32_t pref_net;
                    uint32_t valid_net;

                    memcpy(&pref_net, opt + sub + 16, 4);
                    memcpy(&valid_net, opt + sub + 20, 4);

                    out->preferred_lft = bswap32(pref_net);
                    out->valid_lft = bswap32(valid_net);
                    out->has_addr = true;
                    got_addr = true;
                    break;
                }

                sub += sl;
            }
        }

        if (code == DHCPV6_OPT_IA_PD && len >= 12){
            uint32_t sub = off + 12;
            uint32_t end = off + len;

            while (sub + 4 <= end){
                uint16_t sc_net;
                uint16_t sl_net;

                memcpy(&sc_net, opt + sub + 0, 2);
                memcpy(&sl_net, opt + sub + 2, 2);

                uint16_t sc = bswap16(sc_net);
                uint16_t sl = bswap16(sl_net);

                sub += 4;
                if (sub + sl > end) break;

                if (sc == DHCPV6_OPT_IAPREFIX && sl >= 25){
                    uint32_t pref_net;
                    uint32_t valid_net;

                    memcpy(&pref_net, opt + sub + 0, 4);
                    memcpy(&valid_net, opt + sub + 4, 4);

                    out->pd_preferred_lft = bswap32(pref_net);
                    out->pd_valid_lft = bswap32(valid_net);
                    out->pd_prefix_len = opt[sub + 8];

                    memcpy(out->pd_prefix, opt + sub + 9, 16);

                    out->has_pd = true;
                    break;
                }

                sub += sl;
            }
        }

        off += len;
    }

    return got_addr || out->has_pd || out->has_server_id;
}

bool dhcpv6_parse_message(const uint8_t*msg, uint32_t msg_len, uint32_t expect_xid24, uint32_t expect_iaid, dhcpv6_parsed_t*out){
    if (!msg || !out) return false;
    if (msg_len < 4) return false;

    memset(out, 0, sizeof(*out));

    out->msg_type = msg[0];
    out->xid24 = ((uint32_t)msg[1] << 16) | ((uint32_t)msg[2] << 8) | (uint32_t)msg[3];

    if (expect_xid24 && out->xid24 != (expect_xid24 & 0x00FFFFFFu)) return false;

    return parse_opts(msg + 4, msg_len - 4, expect_iaid, out);
}