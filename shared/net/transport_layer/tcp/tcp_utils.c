#include "tcp_utils.h"

port_manager_t *tcp_pm_for_l3(uint8_t l3_id){
    if (l3_ipv4_find_by_id(l3_id)) return ifmgr_pm_v4(l3_id);
    if (l3_ipv6_find_by_id(l3_id)) return ifmgr_pm_v6(l3_id);
    return NULL;
}

bool tcp_build_tx_opts_from_local_v4(const void *src_ip_addr, ipv4_tx_opts_t *out){
    if (!out) return false;
    uint32_t lip = tcp_v4_u32_from_ptr(src_ip_addr);
    l3_ipv4_interface_t *v4 = l3_ipv4_find_by_ip(lip);
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