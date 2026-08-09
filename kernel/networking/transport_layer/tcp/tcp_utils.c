#include "tcp_utils.h"
#include "std/memory.h"
#include "tcp_internal.h"
#include "networking/interface_manager.h"

#define TCP_INIT_CWND_SEGS 10u

uint32_t tcp_initial_cwnd(uint32_t mss) {
    if (!mss) mss = TCP_DEFAULT_MSS;
    uint32_t ten_mss = mss * TCP_INIT_CWND_SEGS;
    uint32_t lower = 2 * mss;
    if (lower < 14600u) lower = 14600u;
    return ten_mss < lower ? ten_mss : lower;
}

void tcp_update_mss(tcp_flow_t *flow) {
    if (!flow) return;
    uint32_t local_mss = flow->tx.path_mss ? flow->tx.path_mss : TCP_DEFAULT_MSS;
    if (local_mss > TCP_MAX_MSS) local_mss = TCP_MAX_MSS;
    if (flow->tx.configured_mss && flow->tx.configured_mss < local_mss) local_mss = flow->tx.configured_mss;
    flow->tx.advertised_mss = local_mss;

    uint32_t mss = local_mss;
    if (flow->tx.peer_mss && flow->tx.peer_mss < mss) mss = flow->tx.peer_mss;
    flow->tx.mss = mss;
}

uint32_t tcp_calc_mss_for_l3(uint8_t l3_id, ip_version_t ver, const void *remote_ip){
    //TODO propagate icmp4/6 pmtu and errors
    uint32_t mtu = 1500;
    if (ver == IP_VER4) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
        if (v4) mtu = v4->runtime_opts_v4.mtu ? v4->runtime_opts_v4.mtu : 1500;
    } else if (ver == IP_VER6) {
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
        if (v6) mtu = v6->mtu ? v6->mtu : 1500;
    } else return 256;

    if (ver == IP_VER6 && remote_ip){
        uint16_t pmtu = ipv6_pmtu_get((const uint8_t*)remote_ip);
        if (pmtu && pmtu < mtu) mtu = pmtu;
    }

    uint32_t ih = (ver == IP_VER6) ? 40u : 20u;
    uint32_t th = 20u;
    if (mtu <= ih + th) return 256;
    uint32_t mss = mtu - ih - th;
    if (mss < 256u) mss = 256u;
    return mss;
}

void tcp_parse_options(const uint8_t *opts, uint32_t len, tcp_parsed_opts_t *out) {
    if (!out) return;

    out->mss = 0;
    out->wscale = 0;
    out->sack_permitted = 0;
    out->has_mss = 0;
    out->has_wscale = 0;
    out->sack_count = 0;
    memset(out->sacks, 0, sizeof(out->sacks));

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
            out->mss = rd_be16(&opts[i + 2]);
            out->has_mss = 1;
        } else if (kind == 3 && olen == 3) {
            uint8_t ws =opts[i + 2];
            if (ws <= 14) {
                out->wscale = ws;
                out->has_wscale = 1;
            }
        } else if (kind == 4 && olen == 2) {
            out->sack_permitted = 1;
        } else if (kind == 5 && olen >= 10 && ((olen - 2) % 8) == 0) {
            uint32_t blocks = (uint32_t)((olen - 2) / 8);
            if (blocks > TCP_SACK_MAX_BLOCKS) blocks = TCP_SACK_MAX_BLOCKS;

            for (uint32_t b = 0; b < blocks; b++) {
                uint32_t o = i + 2 + b*8;
                uint32_t left  = rd_be32(&opts[o]);
                uint32_t right = rd_be32(&opts[o+4]);
                if (TCP_SEQ_LEQ(right, left)) continue;

                uint8_t n = out->sack_count;
                if (n >= TCP_SACK_MAX_BLOCKS) break;
                out->sacks[n].left = left;
                out->sacks[n].right = right;
                out->sack_count = (uint8_t)(n+1);
            }
        }

        i += olen;
    }
}

uint8_t tcp_build_syn_options(uint8_t *out, uint16_t mss, uint8_t wscale, uint8_t sack_permitted) {
    if (!out) return 0;

    uint8_t i = 0;

    out[i++] = 2;
    out[i++] = 4;
    wr_be16(out + i, mss);
    i += 2;

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