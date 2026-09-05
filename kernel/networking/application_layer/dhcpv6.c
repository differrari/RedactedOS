#include "networking/application_layer/dhcpv6.h"

#include "std/memory.h"
#include "std/string.h"
#include "networking/link_layer/link_utils.h"
#include "networking/internet_layer/ipv6_utils.h"

static bool opt_append(uint8_t* b, uint32_t cap, uint32_t* off, uint16_t code, const void* data, uint16_t len){
    if (!b || !off) return false;
    if (*off + 4u + len > cap) return false;

    uint16_t c = bswap16(code);
    uint16_t l = bswap16(len);

    memcpy(b + *off + 0, &c, 2);
    memcpy(b + *off + 2, &l, 2);

    if (len && data) memcpy(b + *off + 4, data, len);

    *off += 4u + len;
    return true;
}

uint32_t dhcpv6_make_xid24(uint32_t r32){
    uint32_t x = r32 & 0x00FFFFFFu;
    if (!x) x = 1;
    return x;
}

uint32_t dhcpv6_iaid_from_mac(const uint8_t mac[MAC_ADDR_LEN]){
    if (!mac) return 0;
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
}

bool dhcpv6_build_message(uint8_t*out, uint32_t out_cap, uint32_t*out_len, const net_runtime_opts_v6_t*rt, const uint8_t mac[MAC_ADDR_LEN], uint8_t msg_type, uint32_t xid24, uint16_t elapsed_cs, const uint8_t ia_addr[16]) {
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

    if (mac) mac_copy(duid + 4, mac);
    else mac_clear(duid + 4);

    if (!opt_append(out, out_cap, &off, DHCPV6_OPT_CLIENTID, duid, 10)) return false;

    if (msg_type == DHCPV6_MSG_REQUEST || msg_type == DHCPV6_MSG_RENEW || msg_type == DHCPV6_MSG_RELEASE || msg_type == DHCPV6_MSG_DECLINE) {
        if (!rt || !rt->server_id_len || rt->server_id_len > DHCPV6_MAX_SERVER_ID) return false;
        if (!opt_append(out, out_cap, &off, DHCPV6_OPT_SERVERID, rt->server_id, rt->server_id_len)) return false;
    }

    uint16_t elapsed = bswap16(elapsed_cs);
    if (!opt_append(out, out_cap, &off, DHCPV6_OPT_ELAPSED, &elapsed, 2)) return false;

    if (msg_type == DHCPV6_MSG_SOLICIT || msg_type == DHCPV6_MSG_REQUEST || msg_type == DHCPV6_MSG_RENEW ||
        msg_type == DHCPV6_MSG_REBIND || msg_type == DHCPV6_MSG_INFORMATION_REQUEST){
        uint16_t oro[4];
        uint16_t oro_len = 0;

        oro[oro_len++] = bswap16(DHCPV6_OPT_DNS_SERVERS);
        oro[oro_len++] = bswap16(DHCPV6_OPT_NTP_SERVER);

        if (msg_type == DHCPV6_MSG_SOLICIT) oro[oro_len++] = bswap16(DHCPV6_OPT_SOL_MAX_RT);
        if (msg_type == DHCPV6_MSG_INFORMATION_REQUEST) {
            oro[oro_len++] = bswap16(DHCPV6_OPT_INF_MAX_RT);
            if (oro_len < 4) oro[oro_len++] = bswap16(DHCPV6_OPT_INFORMATION_REFRESH_TIME);
        }
        if (!opt_append(out, out_cap, &off, DHCPV6_OPT_ORO, oro, oro_len * 2)) return false;
    }

    if (msg_type == DHCPV6_MSG_SOLICIT || msg_type == DHCPV6_MSG_REQUEST || msg_type == DHCPV6_MSG_CONFIRM || msg_type == DHCPV6_MSG_RENEW || msg_type == DHCPV6_MSG_REBIND || msg_type == DHCPV6_MSG_RELEASE || msg_type == DHCPV6_MSG_DECLINE) {
        uint32_t iaid = rt ? rt->iaid : 0;
        if (!iaid) return false;

        uint8_t payload[40];

        uint32_t iaid_net = bswap32(iaid);
        uint32_t t1_net = 0;
        uint32_t t2_net = 0;

        memcpy(payload, &iaid_net, 4);
        memcpy(payload + 4, &t1_net, 4);
        memcpy(payload + 8, &t2_net, 4);
        uint16_t ia_len = 12;

        if (ia_addr && !ipv6_is_unspecified(ia_addr)) {
            uint16_t code_net = bswap16(DHCPV6_OPT_IAADDR);
            uint16_t len_net = bswap16(24);

            memcpy(payload + 12, &code_net, 2);
            memcpy(payload + 14, &len_net, 2);
            memcpy(payload + 16, ia_addr, 16);
            memcpy(payload + 32, &t1_net, 4);
            memcpy(payload + 36, &t2_net, 4);
            ia_len = 40;
        } else if (msg_type == DHCPV6_MSG_CONFIRM || msg_type == DHCPV6_MSG_RENEW || msg_type == DHCPV6_MSG_REBIND || msg_type == DHCPV6_MSG_RELEASE || msg_type == DHCPV6_MSG_DECLINE) return false;

        if (!opt_append(out, out_cap, &off, DHCPV6_OPT_IA_NA, payload, ia_len)) return false;

        if (rt->pd_prefix_len) {
            uint32_t pd_iaid_net = bswap32(iaid ^ 0xA5A5A5A5u);
            if (msg_type == DHCPV6_MSG_SOLICIT) {
                uint8_t iapd[12];
                memcpy(iapd, &pd_iaid_net, 4);
                memcpy(iapd + 4, &t1_net, 4);
                memcpy(iapd + 8, &t2_net, 4);
                if (!opt_append(out, out_cap, &off, DHCPV6_OPT_IA_PD, iapd, 12)) return false;
            } else {
                uint8_t iapd[41];
                uint16_t code_net = bswap16(DHCPV6_OPT_IAPREFIX);
                uint16_t len_net = bswap16(25);
                uint32_t preferred_net = bswap32(rt->pd_preferred_lft);
                uint32_t valid_net = bswap32(rt->pd_valid_lft);

                memcpy(iapd, &pd_iaid_net, 4);
                memcpy(iapd + 4, &t1_net, 4);
                memcpy(iapd + 8, &t2_net, 4);
                memcpy(iapd + 12, &code_net, 2);
                memcpy(iapd + 14, &len_net, 2);
                memcpy(iapd + 16, &preferred_net, 4);
                memcpy(iapd + 20, &valid_net, 4);

                iapd[24] = rt->pd_prefix_len;
                memcpy(iapd + 25, rt->pd_prefix, 16);

                if (!opt_append(out, out_cap, &off, DHCPV6_OPT_IA_PD, iapd, 41)) return false;
            }
        }
    }
    *out_len = off;
    return true;
}

static bool parse_opts(const uint8_t*opt, uint32_t opt_len, uint32_t expect_iaid, dhcpv6_parsed_t*out){
    uint32_t off = 0;

    while (off < opt_len) {
        if (off + 4 > opt_len) return false;
        uint16_t code_net;
        uint16_t len_net;

        memcpy(&code_net, opt + off + 0, 2);
        memcpy(&len_net, opt + off + 2, 2);

        uint16_t code = bswap16(code_net);
        uint16_t len = bswap16(len_net);

        off += 4;
        if (off + len >opt_len) return false;
        const uint8_t* data = opt + off;

        if (code == DHCPV6_OPT_SERVERID) {
            if (!len || len > DHCPV6_MAX_SERVER_ID || out->has_server_id) return false;
            memcpy(out->server_id, data, len);
            out->server_id_len = len;
            out->has_server_id = true;
        } else if (code == DHCPV6_OPT_CLIENTID) {
            if (!len || len > DHCPV6_MAX_CLIENT_ID || out->has_client_id) return false;
            memcpy(out->client_id, data, len);
            out->client_id_len = len;
            out->has_client_id = true;
        } else if (code == DHCPV6_OPT_STATUS_CODE) {
            if (out->has_status || len < 2) return false;
            uint16_t status_net;
            memcpy(&status_net, data, 2);
            out->status_code = bswap16(status_net);
            out->has_status = true;
        } else if (code == DHCPV6_OPT_DNS_SERVERS) {
            if (len < 16 || (len % 16) != 0) return false;
            int n = (int)(len / 16);
            if (n > 2) n = 2;

            for (int i = 0; i < n; i++) memcpy(out->dns[i], data + (uint32_t)i * 16u, 16);

            out->has_dns = true;
        } else if (code == DHCPV6_OPT_NTP_SERVER) {
            uint32_t ntp_off = 0;
            uint8_t count = 0;
            while (ntp_off < len) {
                if (ntp_off + 4 > len) return false;
                uint16_t sc_net;
                uint16_t sl_net;
                memcpy(&sc_net, data + ntp_off, 2);
                memcpy(&sl_net, data + ntp_off + 2, 2);
                uint16_t sc = bswap16(sc_net);
                uint16_t sl = bswap16(sl_net);
                ntp_off += 4;
                if (ntp_off + sl > len) return false;
                if (sc == DHCPV6_NTP_SUBOPT_SERVER_ADDR && sl == 16 && count < 2) {
                    memcpy(out->ntp[count], data + ntp_off, 16);
                    count++;
                }
                ntp_off += sl;
            }
            out->has_ntp = count != 0;
        } else if (code == DHCPV6_OPT_SOL_MAX_RT || code == DHCPV6_OPT_INF_MAX_RT || code == DHCPV6_OPT_INFORMATION_REFRESH_TIME) {
            if (len != 4) return false;
            uint32_t value_net;
            memcpy(&value_net, data, 4);
            uint32_t value = bswap32(value_net);
            if (code == DHCPV6_OPT_INFORMATION_REFRESH_TIME) {
                out->has_info_refresh_time = true;
                out->info_refresh_time = value;
            } else if (value >= 60 && value <= 86400) {
                if (code == DHCPV6_OPT_SOL_MAX_RT) {
                    out->has_sol_max_rt = true;
                    out->sol_max_rt = value;
                } else {
                    out->has_inf_max_rt = true;
                    out->inf_max_rt = value;
                }
            }
        } else if (code == DHCPV6_OPT_IA_NA) {
            if (len < 12) return false;
            uint32_t iaid_net;
            uint32_t t1_net;
            uint32_t t2_net;

            memcpy(&iaid_net, data, 4);
            memcpy(&t1_net, data + 4, 4);
            memcpy(&t2_net, data + 8, 4);

            uint32_t iaid = bswap32(iaid_net);
            if (!expect_iaid || iaid == expect_iaid) {
                uint32_t t1 = bswap32(t1_net);
                uint32_t t2 = bswap32(t2_net);
                if (t1 && t2 && t1 > t2) {
                    off += len;
                    continue;
                }
                out->has_ia_na = true;
                out->t1 = t1;
                out->t2 = t2;

                uint32_t sub = 12;
                while (sub < len) {
                    if (sub + 4 > len) return false;
                    uint16_t sc_net;
                    uint16_t sl_net;

                    memcpy(&sc_net, data + sub, 2);
                    memcpy(&sl_net, data + sub + 2, 2);

                    uint16_t sc = bswap16(sc_net);
                    uint16_t sl = bswap16(sl_net);

                    sub += 4;
                    if (sub + sl > len) return false;

                    if (sc == DHCPV6_OPT_STATUS_CODE) {
                        if (out->has_ia_status || sl < 2) return false;
                        uint16_t status_net;
                        memcpy(&status_net, data + sub, 2);
                        out->ia_status_code = bswap16(status_net);
                        out->has_ia_status = true;
                    } else if (sc == DHCPV6_OPT_IAADDR && sl >= 24 && !out->has_addr) {
                        uint32_t pref_net;
                        uint32_t valid_net;

                        memcpy(&pref_net, data + sub + 16, 4);
                        memcpy(&valid_net, data + sub + 20, 4);

                        uint32_t preferred = bswap32(pref_net);
                        uint32_t valid = bswap32(valid_net);
                        if (preferred <= valid && !ipv6_is_unspecified(data + sub)) {
                            if (valid) {
                                memcpy(out->addr, data + sub, 16);
                                out->preferred_lft = preferred;
                                out->valid_lft = valid;
                                out->has_addr = true;
                            } else if (!out->has_expired_addr) {
                                memcpy(out->expired_addr, data + sub, 16);
                                out->has_expired_addr = true;
                            }
                        }
                    }
                    sub += sl;
                }
            }
        } else if (code == DHCPV6_OPT_IA_PD){
            if (len < 12) return false;
            uint32_t iaid_net;
            memcpy(&iaid_net, data, 4);
            uint32_t iaid = bswap32(iaid_net);
            uint32_t expect_pd_iaid = expect_iaid ? (expect_iaid ^ 0xA5A5A5A5u) : 0;

            if (!expect_pd_iaid || iaid == expect_pd_iaid) {
                uint32_t sub = 12;
                while (sub < len) {
                    if (sub + 4 > len) return false;
                    uint16_t sc_net;
                    uint16_t sl_net;

                    memcpy(&sc_net, data + sub, 2);
                    memcpy(&sl_net, data + sub + 2, 2);

                    uint16_t sc = bswap16(sc_net);
                    uint16_t sl = bswap16(sl_net);

                    sub += 4;
                    if (sub + sl > len) return false;

                    if (sc == DHCPV6_OPT_IAPREFIX && sl >= 25 && !out->has_pd) {
                        uint32_t pref_net;
                        uint32_t valid_net;

                        memcpy(&pref_net, data + sub, 4);
                        memcpy(&valid_net, data + sub + 4, 4);

                        uint32_t preferred = bswap32(pref_net);
                        uint32_t valid = bswap32(valid_net);
                        uint8_t prefix_len = data[sub + 8];

                        if (preferred <= valid && prefix_len <= 128 && valid) {
                            out->pd_preferred_lft = preferred;
                            out->pd_valid_lft = valid;
                            out->pd_prefix_len = prefix_len;
                            memcpy(out->pd_prefix, data + sub + 9, 16);

                            out->has_pd = true;
                        }
                    }
                    sub += sl;
                }
            }
        }

        off += len;
    }

    return true;
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