#include "tcp_utils.h"

uint32_t tcp_calc_mss_for_l3(uint8_t l3_id, ip_version_t ver, const void *remote_ip){
    uint32_t mtu = 1500;
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
    if (v6) mtu =v6->mtu ? v6->mtu : 1500;

    l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
    if (v4)  mtu = v4->runtime_opts_v4.mtu ? v4->runtime_opts_v4.mtu : 1500;

    if (ver == IP_VER6 && remote_ip){
        uint16_t pmtu =ipv6_pmtu_get((const uint8_t*)remote_ip);
        if (pmtu && pmtu < mtu) mtu = pmtu;
    }

    uint32_t ih = (ver == IP_VER6) ? 40u : 20u;
    uint32_t th = 20u;
    if (mtu <= ih + th) return 256;
    uint32_t mss = mtu - ih - th;
    if (mss < 256u) mss = 256u;
    return mss;
}

bool tcp_build_tx_opts_from_local_v4(const void *src_ip_addr, ipv4_tx_opts_t *out){
    if (!out) return false;
    l3_ipv4_interface_t *v4 = l3_ipv4_find_by_ip(*(const uint32_t *)src_ip_addr);
    if (v4) {
        out->scope = IP_TX_BOUND_L3;
        out->index = v4->l3_id;
    } else {
        out->scope = IP_TX_AUTO;
        out->index = 0;
    }
    return true;
}

bool tcp_build_tx_opts_from_l3(uint8_t l3_id, ipv4_tx_opts_t *out){
    if (!out) return false;
    out->scope = IP_TX_BOUND_L3;
    out->index = l3_id;
    return true;
}

bool tcp_build_tx_opts_from_local_v6(const void *src_ip_addr, ipv6_tx_opts_t *out){
    if (!out) return false;
    const uint8_t *sip = (const uint8_t *)src_ip_addr;
    l3_ipv6_interface_t *v6 = l3_ipv6_find_by_ip(sip);
    if (v6 && v6->l2) {
        out->scope = IP_TX_BOUND_L3;
        out->index = v6->l3_id;
    } else {
        out->scope = IP_TX_AUTO;
        out->index = 0;
    }
    return true;
}

void tcp_parse_options(const uint8_t *opts, uint32_t len, tcp_parsed_opts_t *out) {
    if (!out) return;

    out->mss = 0;
    out->wscale = 0;
    out->sack_permitted = 0;
    out->has_mss = 0;
    out->has_wscale = 0;

    if (!opts || len == 0) return;

    uint32_t i = 0;
    while (i < len){
        uint8_t kind = opts[i];
        if (kind == 0) break;
        if (kind == 1) {
            i++;
            continue;
        }

        if (i + 1 >= len) break;
        uint8_t olen = opts[i + 1];
        if (olen < 2) break;
        if (i + olen > len) break;

        if (kind == 2 && olen == 4) {
            out->mss = (uint16_t)((opts[i + 2] << 8) | opts[i + 3]);
            out->has_mss = 1;
        } else if (kind == 3 && olen == 3) {
            out->wscale =opts[i + 2];
            out->has_wscale = 1;
        } else if (kind == 4 && olen == 2) {
            out->sack_permitted = 1;
        }

        i += olen;
    }
}

uint8_t tcp_build_syn_options(uint8_t *out, uint16_t mss, uint8_t wscale, uint8_t sack_permitted) {
    if (!out) return 0;

    uint8_t i = 0;

    out[i++] = 2;
    out[i++] = 4;
    out[i++] = (uint8_t)(mss >> 8);
    out[i++] = (uint8_t)(mss & 0xff);

    if (wscale != 0xffu){
        out[i++] = 1;
        out[i++] = 3;
        out[i++] = 3;
        out[i++] = wscale;
    }

    if (sack_permitted){
        out[i++] = 1;
        out[i++] = 1;
        out[i++] = 4;
        out[i++] = 2;
    }

    while (i & 3) out[i++] = 1;

    return i;
}