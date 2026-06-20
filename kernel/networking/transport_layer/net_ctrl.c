#include "net_ctrl.h"
#include "files/buffer.h"
#include "net/socket_types.h"
#include "std/memory.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_route.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/link_layer/arp.h"
#include "networking/link_layer/ndp.h"
#include "networking/network.h"

typedef struct {
    uint8_t ifindex;
    uint8_t l3_id;
    uint8_t prefix_len;
    uint8_t ifname_len;
    char ifname[16];
    uint8_t mac[6];
    uint16_t metric;
    uint16_t mtu;
    uint16_t flags;
    int16_t config;
    uint8_t state;
    uint8_t kind;
    uint8_t dad_state;
    uint32_t ttl_ms;
    uint32_t present;
    net_l4_endpoint address;
    net_l4_endpoint gateway;
} net_ctrl_attrs_t;

static bool net_ctrl_read_attrs(const uint8_t* p, uint32_t len, net_ctrl_attrs_t* out) {
    if (!out)return false;
    memset(out, 0, sizeof(*out));
    uint32_t off = 0;
    while (off < len) {
        if (len - off < sizeof(NetCtrlAttr)) return false;
        const NetCtrlAttr* attr = (const NetCtrlAttr*)(p + off);
        NetCtrlAttr a;
        memcpy(&a, attr, sizeof(a));
        off += sizeof(a);
        if (a.length > len - off) return false;
        const uint8_t* v = NET_CTRL_ATTR_CONST_DATA(attr);
        switch ((uint32_t)a.ext) {
            case NET_CTRL_EXT_IFINDEX:
                if (a.length != sizeof(uint8_t)) return false;
                out->ifindex = v[0];
                out->present |= 1u << NET_CTRL_EXT_IFINDEX;
                break;
            case NET_CTRL_EXT_IFNAME:
                if (!a.length || a.length > sizeof(out->ifname)) return false;
                memcpy(out->ifname, v, a.length);
                out->ifname_len = (uint8_t)a.length;
                out->present |= 1u << NET_CTRL_EXT_IFNAME;
                break;
            case NET_CTRL_EXT_L3_ID:
                if (a.length != sizeof(uint8_t)) return false;
                out->l3_id = v[0];
                out->present |= 1u << NET_CTRL_EXT_L3_ID;
                break;
            case NET_CTRL_EXT_PREFIX_LEN:
                if (a.length != sizeof(uint8_t)) return false;
                out->prefix_len = v[0];
                out->present |= 1u << NET_CTRL_EXT_PREFIX_LEN;
                break;
            case NET_CTRL_EXT_METRIC:
                if (a.length != sizeof(uint16_t)) return false;
                memcpy(&out->metric, v, sizeof(out->metric));
                out->present |= 1u << NET_CTRL_EXT_METRIC;
                break;
            case NET_CTRL_EXT_MTU:
                if (a.length != sizeof(uint16_t)) return false;
                memcpy(&out->mtu, v, sizeof(out->mtu));
                out->present |= 1u << NET_CTRL_EXT_MTU;
                break;
            case NET_CTRL_EXT_FLAGS:
                if (a.length != sizeof(uint16_t)) return false;
                memcpy(&out->flags, v, sizeof(out->flags));
                out->present |= 1u << NET_CTRL_EXT_FLAGS;
                break;
            case NET_CTRL_EXT_STATE:
                if (a.length != sizeof(uint8_t)) return false;
                out->state = v[0];
                out->present |= 1u << NET_CTRL_EXT_STATE;
                break;
            case NET_CTRL_EXT_CONFIG:
                if (a.length != sizeof(int16_t)) return false;
                memcpy(&out->config, v, sizeof(out->config));
                out->present |= 1u << NET_CTRL_EXT_CONFIG;
                break;
            case NET_CTRL_EXT_KIND:
                if (a.length != sizeof(uint8_t)) return false;
                out->kind = v[0];
                out->present |= 1u << NET_CTRL_EXT_KIND;
                break;
            case NET_CTRL_EXT_DAD_STATE:
                if (a.length != sizeof(uint8_t)) return false;
                out->dad_state = v[0];
                out->present |= 1u << NET_CTRL_EXT_DAD_STATE;
                break;
            case NET_CTRL_EXT_TTL_MS:
                if (a.length != sizeof(uint32_t)) return false;
                memcpy(&out->ttl_ms, v, sizeof(out->ttl_ms));
                out->present |= 1u << NET_CTRL_EXT_TTL_MS;
                break;
            case NET_CTRL_EXT_ADDRESS:
                if (a.length != sizeof(net_l4_endpoint)) return false;
                memcpy(&out->address, v, sizeof(out->address));
                out->present |= 1u << NET_CTRL_EXT_ADDRESS;
                break;
            case NET_CTRL_EXT_GATEWAY:
                if (a.length != sizeof(net_l4_endpoint)) return false;
                memcpy(&out->gateway, v, sizeof(out->gateway));
                out->present |= 1u << NET_CTRL_EXT_GATEWAY;
                break;
            case NET_CTRL_EXT_MAC:
                if (a.length != sizeof(out->mac)) return false;
                memcpy(out->mac, v, sizeof(out->mac));
                out->present |= 1u << NET_CTRL_EXT_MAC;
                break;
            //case NET_CTRL_EXT_NONE:
            default:
                break;
        }
        off += a.length;
    }
    return true;
}

static bool net_ctrl_ifname_matches(const l2_interface_t* l2, const net_ctrl_attrs_t* a) {
    if (!NET_CTRL_HAS(a, NET_CTRL_EXT_IFNAME)) return true;
    if (memcmp(l2->name, a->ifname, a->ifname_len) != 0) return false;
    return a->ifname_len == sizeof(a->ifname) || l2->name[a->ifname_len] == '\0';
}

static inline bool net_ctrl_l2_matches(const l2_interface_t* l2, const net_ctrl_attrs_t* a) {
    if (!l2) return false;
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_IFINDEX) && l2->ifindex != a->ifindex) return false;
    return net_ctrl_ifname_matches(l2, a);
}

static l2_interface_t* net_ctrl_l2_from_attrs(const net_ctrl_attrs_t* a) {
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_IFINDEX)) {
        l2_interface_t* l2 = l2_interface_find_by_index(a->ifindex);
        if (!l2 || !net_ctrl_ifname_matches(l2, a)) return NULL;
        return l2;
    }
    if (!NET_CTRL_HAS(a, NET_CTRL_EXT_IFNAME)) return NULL;
    uint8_t count = l2_interface_count();
    for (uint8_t i = 0; i < count; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (l2 && net_ctrl_ifname_matches(l2, a)) return l2;
    }
    return NULL;
}

static bool net_ctrl_link_dump(const net_ctrl_attrs_t* a, buffer* b) {
    uint8_t count = l2_interface_count();
    for (uint8_t i = 0; i < count; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!net_ctrl_l2_matches(l2, a)) continue;
        NetCtrlLinkInfo info;
        memset(&info, 0, sizeof(info));
        info.ifindex = l2->ifindex;
        info.up = l2->is_up ? 1 : 0;
        info.metric = l2->base_metric;
        info.mtu = network_get_mtu(l2->ifindex);
        info.kind = l2->kind;
        info.ipv4_count = l2->ipv4_count;
        info.ipv6_count = l2->ipv6_count;
        memcpy(info.name, l2->name, sizeof(info.name));
        if (buffer_write_lim(b, (const char*)&info, sizeof(info)) != sizeof(info)) return false;
    }
    return true;
}

static int32_t net_ctrl_link_upd(const net_ctrl_attrs_t* a) {
    l2_interface_t* l2 = net_ctrl_l2_from_attrs(a);
    if (!l2) return SOCK_ERR_INVAL;
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_MTU)) return SOCK_ERR_UNSUP;
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_STATE) && !l2_interface_set_up(l2->ifindex, a->state != 0)) return SOCK_ERR_INVAL;
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_METRIC) && !l2_interface_set_metric(l2->ifindex, a->metric)) return SOCK_ERR_INVAL;
    return SOCK_OK;
}

static bool net_ctrl_addr_dump(const net_ctrl_attrs_t* a, buffer* b) {
    uint8_t count = l2_interface_count();
    for (uint8_t i = 0; i < count; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!net_ctrl_l2_matches(l2, a)) continue;
        for (int j = 0; j < MAX_IPV4_PER_INTERFACE; j++) {
            l3_ipv4_interface_t* v4 = l2->l3_v4[j];
            if (!v4) continue;
            if (NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID) && v4->l3_id != a->l3_id) continue;
            NetCtrlAddrInfo info;
            memset(&info, 0, sizeof(info));
            info.prefix.ifindex = l2->ifindex;
            info.prefix.l3_id = v4->l3_id;
            int prefix_len = ipv4_prefix_len(v4->mask);
            info.prefix.prefix_len = prefix_len < 0 ? 0 : (uint8_t)prefix_len;
            info.config = v4->mode;
            info.epoch = v4->epoch;
            info.mtu = v4->runtime_opts_v4.mtu ? v4->runtime_opts_v4.mtu : network_get_mtu(l2->ifindex);
            info.prefix.address.ver = IP_VER4;
            info.prefix.gateway.ver = IP_VER4;
            memcpy(info.prefix.address.ip, &v4->ip, sizeof(v4->ip));
            memcpy(info.prefix.gateway.ip, &v4->gw, sizeof(v4->gw));
            if (buffer_write_lim(b, (const char*)&info, sizeof(info)) != sizeof(info)) return false;
        }
        for (int j = 0; j < MAX_IPV6_PER_INTERFACE; j++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[j];
            if (!v6) continue;
            if (NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID) && v6->l3_id != a->l3_id) continue;
            NetCtrlAddrInfo info;
            memset(&info, 0, sizeof(info));
            info.prefix.ifindex = l2->ifindex;
            info.prefix.l3_id = v6->l3_id;
            info.prefix.prefix_len = v6->prefix_len;
            info.kind = v6->kind;
            info.config = v6->cfg;
            info.epoch = v6->epoch;
            info.mtu = v6->mtu ? v6->mtu : network_get_mtu(l2->ifindex);
            info.dad_state = v6->dad_state;
            info.prefix.address.ver = IP_VER6;
            info.prefix.gateway.ver = IP_VER6;
            ipv6_cpy(info.prefix.address.ip, v6->ip);
            ipv6_cpy(info.prefix.gateway.ip, v6->gateway);
            if (buffer_write_lim(b, (const char*)&info, sizeof(info)) != sizeof(info)) return false;
        }
    }
    return true;
}

static int32_t net_ctrl_addr_apply(const net_ctrl_attrs_t* a, bool update) {
    if (!NET_CTRL_HAS(a, NET_CTRL_EXT_ADDRESS)) return SOCK_ERR_INVAL;
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_MTU)) return SOCK_ERR_UNSUP;
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_DAD_STATE)) return SOCK_ERR_UNSUP;
    if (update && !NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID)) return SOCK_ERR_INVAL;
    l2_interface_t* l2 = update ? NULL : net_ctrl_l2_from_attrs(a);
    if (!update && (!l2 || NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID))) return SOCK_ERR_INVAL;
    if (a->address.ver == IP_VER4) {
        uint32_t ip = 0;
        uint32_t gw = 0;
        int16_t config = NET_CTRL_HAS(a, NET_CTRL_EXT_CONFIG) ? a->config : IPV4_CFG_STATIC;
        if ((config != IPV4_CFG_DISABLED && config != IPV4_CFG_DHCP && config != IPV4_CFG_STATIC) || NET_CTRL_HAS(a, NET_CTRL_EXT_KIND)) return SOCK_ERR_INVAL;
        memcpy(&ip, a->address.ip, sizeof(ip));
        if (NET_CTRL_HAS(a, NET_CTRL_EXT_GATEWAY)) {
            if (a->gateway.ver != IP_VER4) return SOCK_ERR_INVAL;
            memcpy(&gw, a->gateway.ip, sizeof(gw));
        }
        if (config == IPV4_CFG_STATIC && (!NET_CTRL_HAS(a, NET_CTRL_EXT_PREFIX_LEN) || a->prefix_len > 32)) return SOCK_ERR_INVAL;
        if (config != IPV4_CFG_STATIC && NET_CTRL_HAS(a, NET_CTRL_EXT_PREFIX_LEN) && a->prefix_len > 32) return SOCK_ERR_INVAL;
        uint32_t mask = 0;
        if (config == IPV4_CFG_STATIC) {
            if (a->prefix_len >= 32) mask = 0xFFFFFFFF;
            else if (a->prefix_len) mask = 0xFFFFFFFF << (32 - a->prefix_len);
        }
        if (config != IPV4_CFG_STATIC) {
            ip = 0;
            gw = 0;
        }
        if (update) return l3_ipv4_update(a->l3_id, ip, mask, gw, (ipv4_cfg_t)config, NULL) ? SOCK_OK : SOCK_ERR_INVAL;
        return l3_ipv4_add_to_interface(l2->ifindex, ip, mask, gw, (ipv4_cfg_t)config, NULL) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    if (a->address.ver == IP_VER6) {
        int16_t config = NET_CTRL_HAS(a, NET_CTRL_EXT_CONFIG) ? a->config : IPV6_CFG_STATIC;
        uint8_t kind = NET_CTRL_HAS(a, NET_CTRL_EXT_KIND) ? a->kind : (ipv6_is_linklocal(a->address.ip) ? IPV6_ADDRK_LINK_LOCAL : IPV6_ADDRK_GLOBAL);
        if (config != IPV6_CFG_DISABLE && config != IPV6_CFG_STATIC && config != IPV6_CFG_SLAAC && config != IPV6_CFG_DHCPV6) return SOCK_ERR_INVAL;
        if (kind != IPV6_ADDRK_GLOBAL && kind != IPV6_ADDRK_LINK_LOCAL) return SOCK_ERR_INVAL;
        if (config == IPV6_CFG_STATIC && (!NET_CTRL_HAS(a, NET_CTRL_EXT_PREFIX_LEN) || a->prefix_len > 128)) return SOCK_ERR_INVAL;
        if (config != IPV6_CFG_STATIC && NET_CTRL_HAS(a, NET_CTRL_EXT_PREFIX_LEN) && a->prefix_len > 128) return SOCK_ERR_INVAL;
        if (NET_CTRL_HAS(a, NET_CTRL_EXT_GATEWAY) && a->gateway.ver != IP_VER6) return SOCK_ERR_INVAL;
        const uint8_t* gw = NET_CTRL_HAS(a, NET_CTRL_EXT_GATEWAY) ? a->gateway.ip : (const uint8_t[16]){0};
        uint8_t prefix_len = NET_CTRL_HAS(a, NET_CTRL_EXT_PREFIX_LEN) ? a->prefix_len : 0;
        if (update) return l3_ipv6_update(a->l3_id, a->address.ip, prefix_len, gw, (ipv6_cfg_t)config, kind) ? SOCK_OK : SOCK_ERR_INVAL;
        return l3_ipv6_add_to_interface(l2->ifindex, a->address.ip, prefix_len, gw, (ipv6_cfg_t)config, kind) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    return SOCK_ERR_INVAL;
}

static int32_t net_ctrl_addr_del(const net_ctrl_attrs_t* a) {
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID)) {
        if (l3_is_v6_from_id(a->l3_id)) return l3_ipv6_remove_from_interface(a->l3_id) ? SOCK_OK : SOCK_ERR_INVAL;
        return l3_ipv4_remove_from_interface(a->l3_id) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    if (!NET_CTRL_HAS(a, NET_CTRL_EXT_ADDRESS)) return SOCK_ERR_INVAL;
    if (a->address.ver == IP_VER4) {
        uint32_t ip = 0;
        memcpy(&ip, a->address.ip, sizeof(ip));
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(ip);
        return v4 && l3_ipv4_remove_from_interface(v4->l3_id) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    if (a->address.ver == IP_VER6) {
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_ip(a->address.ip);
        return v6 && l3_ipv6_remove_from_interface(v6->l3_id) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    return SOCK_ERR_INVAL;
}

static bool net_ctrl_route_dump(const net_ctrl_attrs_t* a, buffer* b) {
    uint8_t count = l2_interface_count();
    for (uint8_t i = 0; i < count; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!net_ctrl_l2_matches(l2, a)) continue;
        for (int j = 0; j < MAX_IPV4_PER_INTERFACE; j++) { 
            l3_ipv4_interface_t* v4 = l2->l3_v4[j];
            if (!v4 || !v4->routing_table) continue;
            if (NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID) && v4->l3_id != a->l3_id) continue;
            int n = ipv4_rt_count((const ipv4_rt_table_t*)v4->routing_table);
            for (int r = 0; r < n; r++) {
                ipv4_rt_entry_t e;
                if (!ipv4_rt_get((const ipv4_rt_table_t*)v4->routing_table, r, &e)) continue;
                NetCtrlRouteInfo info;
                memset(&info, 0, sizeof(info));
                info.prefix.ifindex = l2->ifindex;
                info.prefix.l3_id = v4->l3_id;
                int prefix_len = ipv4_prefix_len(e.mask);
                info.prefix.prefix_len = prefix_len < 0 ? 0 : (uint8_t)prefix_len;
                info.metric = e.metric;
                info.route_epoch = ipv4_rt_epoch((const ipv4_rt_table_t*)v4->routing_table);
                info.prefix.address.ver = IP_VER4;
                info.prefix.gateway.ver = IP_VER4;
                memcpy(info.prefix.address.ip, &e.network, sizeof(e.network));
                memcpy(info.prefix.gateway.ip, &e.gateway, sizeof(e.gateway));
                if (buffer_write_lim(b, (const char*)&info, sizeof(info)) != sizeof(info)) return false;
            }
        }
        for (int j = 0; j < MAX_IPV6_PER_INTERFACE; j++) {
            l3_ipv6_interface_t* v6 = l2->l3_v6[j];
            if (!v6 || !v6->routing_table) continue;
            if (NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID) && v6->l3_id != a->l3_id) continue;
            int n = ipv6_rt_count((const ipv6_rt_table_t*)v6->routing_table);
            for (int r = 0; r < n; r++) {
                ipv6_rt_entry_t e;
                if (!ipv6_rt_get((const ipv6_rt_table_t*)v6->routing_table, r, &e)) continue;
                NetCtrlRouteInfo info;
                memset(&info, 0, sizeof(info));
                info.prefix.ifindex = l2->ifindex;
                info.prefix.l3_id = v6->l3_id;
                info.prefix.prefix_len = e.prefix_len;
                info.metric = e.metric;
                info.route_epoch = ipv6_rt_epoch((const ipv6_rt_table_t*)v6->routing_table);
                info.prefix.address.ver = IP_VER6;
                info.prefix.gateway.ver = IP_VER6;
                ipv6_cpy(info.prefix.address.ip, e.network);
                ipv6_cpy(info.prefix.gateway.ip, e.gateway);
                if (buffer_write_lim(b, (const char*)&info, sizeof(info)) != sizeof(info)) return false;
            }
        }
    }
    return true;
}

static int32_t net_ctrl_route_apply(const net_ctrl_attrs_t* a, bool add) {
    if (!NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID) || !NET_CTRL_HAS(a, NET_CTRL_EXT_ADDRESS) || !NET_CTRL_HAS(a, NET_CTRL_EXT_PREFIX_LEN)) return SOCK_ERR_INVAL;
    uint16_t metric = NET_CTRL_HAS(a, NET_CTRL_EXT_METRIC) ? a->metric : 0;
    if (!l3_is_v6_from_id(a->l3_id)) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(a->l3_id);
        uint32_t network = 0;
        uint32_t gw = 0;
        if (!v4 || v4->is_localhost || a->address.ver != IP_VER4) return SOCK_ERR_INVAL;
        memcpy(&network, a->address.ip, sizeof(network));
        if (a->prefix_len > 32) return SOCK_ERR_INVAL;
        if (NET_CTRL_HAS(a, NET_CTRL_EXT_GATEWAY)) {
            if (a->gateway.ver != IP_VER4) return SOCK_ERR_INVAL;
            memcpy(&gw, a->gateway.ip, sizeof(gw));
        }
        if (!v4->routing_table && !add) return SOCK_ERR_NOT_FOUND;
        if (!v4->routing_table) v4->routing_table = ipv4_rt_create(v4->l3_id);
        if (!v4->routing_table) return SOCK_ERR_SYS;
        uint32_t mask = 0;
        if (a->prefix_len >= 32) mask = 0xFFFFFFFF;
        else if (a->prefix_len) mask = 0xFFFFFFFF << (32 -a->prefix_len);
        bool exists = false;
        int n = ipv4_rt_count((const ipv4_rt_table_t*)v4->routing_table);
        for (int i = 0; i < n; i++) {
            ipv4_rt_entry_t e;
            if (ipv4_rt_get((const ipv4_rt_table_t*)v4->routing_table, i, &e) && e.network == (network & mask) && e.mask == mask) {
                exists = true;
                break;
            }
        }
        if (add && exists) return SOCK_ERR_EXIST;
        if (!add && !exists) return SOCK_ERR_NOT_FOUND;
        return ipv4_rt_add_in((ipv4_rt_table_t*)v4->routing_table, network & mask, mask, gw, metric) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(a->l3_id);
    if (!v6 || v6->is_localhost || a->address.ver != IP_VER6 || a->prefix_len > 128) return SOCK_ERR_INVAL;
    if (NET_CTRL_HAS(a, NET_CTRL_EXT_GATEWAY) && a->gateway.ver != IP_VER6) return SOCK_ERR_INVAL;
    const uint8_t* gw = NET_CTRL_HAS(a, NET_CTRL_EXT_GATEWAY) ? a->gateway.ip : (const uint8_t[16]){0};
    if (!v6->routing_table && !add) return SOCK_ERR_NOT_FOUND;
    if (!v6->routing_table) v6->routing_table = ipv6_rt_create(v6->l3_id);
    if (!v6->routing_table) return SOCK_ERR_SYS;
    uint8_t net[16];
    ipv6_prefix_network(a->address.ip, a->prefix_len, net);
    bool exists = false;
    int n = ipv6_rt_count((const ipv6_rt_table_t*)v6->routing_table);
    for (int i = 0; i < n; i++) {
        ipv6_rt_entry_t e;
        if (ipv6_rt_get((const ipv6_rt_table_t*)v6->routing_table, i, &e) && e.prefix_len == a->prefix_len && ipv6_cmp(e.network, net) == 0) {
            exists = true;
            break;
        }
    }
    if (add && exists) return SOCK_ERR_EXIST;
    if (!add && !exists) return SOCK_ERR_NOT_FOUND;
    return ipv6_rt_add_in((ipv6_rt_table_t*)v6->routing_table, net, a->prefix_len, gw, metric) ? SOCK_OK : SOCK_ERR_INVAL;
}

static int32_t net_ctrl_route_del(const net_ctrl_attrs_t* a) {
    if (!NET_CTRL_HAS(a, NET_CTRL_EXT_L3_ID) || !NET_CTRL_HAS(a, NET_CTRL_EXT_ADDRESS) || !NET_CTRL_HAS(a, NET_CTRL_EXT_PREFIX_LEN)) return SOCK_ERR_INVAL;
    if (!l3_is_v6_from_id(a->l3_id)) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(a->l3_id);
        uint32_t network = 0;
        if (!v4 || !v4->routing_table || a->address.ver != IP_VER4) return SOCK_ERR_INVAL;
        memcpy(&network, a->address.ip, sizeof(network));
        if (a->prefix_len > 32) return SOCK_ERR_INVAL;
        uint32_t mask = 0;
        if (a->prefix_len >= 32) mask = 0xFFFFFFFF;
        else if (a->prefix_len) mask = 0xFFFFFFFF << (32 -(a->prefix_len));
        return ipv4_rt_del_in((ipv4_rt_table_t*)v4->routing_table, network & mask, mask) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(a->l3_id);
    if (!v6 || !v6->routing_table || a->address.ver != IP_VER6 || a->prefix_len > 128) return SOCK_ERR_INVAL;
    uint8_t net[16];
    ipv6_prefix_network(a->address.ip, a->prefix_len, net);
    return ipv6_rt_del_in((ipv6_rt_table_t*)v6->routing_table, net, a->prefix_len) ? SOCK_OK : SOCK_ERR_INVAL;
}

static bool net_ctrl_neigh_dump(const net_ctrl_attrs_t* a, buffer* b) {
    uint8_t count = l2_interface_count();
    for (uint8_t i = 0; i < count; i++) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!net_ctrl_l2_matches(l2, a)) continue;
        arp_entry_t ae[ARP_TABLE_MAX];
        uint32_t an = arp_table_dump_for_l2(l2->ifindex, ae, ARP_TABLE_MAX);
        for (uint32_t j = 0; j < an; j++) {
            if (NET_CTRL_HAS(a, NET_CTRL_EXT_ADDRESS)) {
                uint32_t want = 0;
                if (a->address.ver != IP_VER4) continue;
                memcpy(&want, a->address.ip, sizeof(want));
                if (ae[j].ip != want) continue;
            }
            NetCtrlNeighInfo info;
            memset(&info, 0, sizeof(info));
            info.ifindex = l2->ifindex;
            info.state = ae[j].state;
            info.ttl_ms = ae[j].ttl_ms;
            info.router_lifetime_ms = 0;
            if (ae[j].static_entry) info.flags |= NET_CTRL_NEIGH_F_STATIC;
            info.address.ver = IP_VER4;
            memcpy(info.address.ip, &ae[j].ip, sizeof(ae[j].ip));
            memcpy(info.mac, ae[j].mac, 6);
            if (buffer_write_lim(b, (const char*)&info, sizeof(info)) != sizeof(info)) return false;
        }
        ndp_entry_t ne[NDP_TABLE_MAX];
        uint32_t nn = ndp_table_dump_for_l2(l2->ifindex, ne, NDP_TABLE_MAX);
        for (uint32_t j = 0; j < nn; j++) {
            if (NET_CTRL_HAS(a, NET_CTRL_EXT_ADDRESS)) {
                if (a->address.ver != IP_VER6 || ipv6_cmp(a->address.ip, ne[j].ip) != 0) continue;
            }
            NetCtrlNeighInfo info;
            memset(&info, 0, sizeof(info));
            info.ifindex = l2->ifindex;
            info.state = ne[j].state;
            info.ttl_ms = ne[j].ttl_ms;
            info.router_lifetime_ms = ne[j].router_lifetime_ms;
            if (ne[j].static_entry) info.flags |= NET_CTRL_NEIGH_F_STATIC;
            if (ne[j].is_router) info.flags |= NET_CTRL_NEIGH_F_ROUTER;
            info.address.ver = IP_VER6;
            ipv6_cpy(info.address.ip, ne[j].ip);
            memcpy(info.mac, ne[j].mac, 6);
            if (buffer_write_lim(b, (const char*)&info, sizeof(info)) != sizeof(info)) return false;
        }
    }
    return true;
}

static int32_t net_ctrl_neigh_set(const net_ctrl_attrs_t* a, bool add) {
    if (!NET_CTRL_HAS(a, NET_CTRL_EXT_ADDRESS) || !NET_CTRL_HAS(a, NET_CTRL_EXT_MAC)) return SOCK_ERR_INVAL;
    l2_interface_t* l2 = net_ctrl_l2_from_attrs(a);
    if (!l2) return SOCK_ERR_INVAL;
    bool exists = false;
    if (a->address.ver == IP_VER4) {
        uint32_t ip = 0;
        memcpy(&ip, a->address.ip, sizeof(ip));
        arp_entry_t ae[ARP_TABLE_MAX];
        uint32_t n = arp_table_dump_for_l2(l2->ifindex, ae, ARP_TABLE_MAX);
        for (uint32_t i = 0; i < n; i++) {
            if (ae[i].ip == ip) {
                exists = true;
                break;
            }
        }
        if (add && exists) return SOCK_ERR_EXIST;
        if (!add && !exists) return SOCK_ERR_NOT_FOUND;
        if (!l2->arp_table) return SOCK_ERR_INVAL;
        uint32_t ttl = NET_CTRL_HAS(a, NET_CTRL_EXT_TTL_MS) ? a->ttl_ms : 0;
        arp_table_put_for_l2(l2->ifindex, ip, a->mac, ttl, NET_CTRL_HAS(a, NET_CTRL_EXT_FLAGS) && (a->flags & NET_CTRL_NEIGH_F_STATIC));
        return SOCK_OK;
    }
    if (a->address.ver == IP_VER6) {
        ndp_entry_t ne[NDP_TABLE_MAX];
        uint32_t n = ndp_table_dump_for_l2(l2->ifindex, ne, NDP_TABLE_MAX);
        for (uint32_t i = 0; i < n; i++) {
            if (ipv6_cmp(ne[i].ip, a->address.ip) == 0) {
                exists = true;
                break;
            }
        }
        if (add && exists) return SOCK_ERR_EXIST;
        if (!add && !exists) return SOCK_ERR_NOT_FOUND;
        if (!l2->nd_table) return SOCK_ERR_INVAL;
        uint32_t ttl = NET_CTRL_HAS(a, NET_CTRL_EXT_TTL_MS) ? a->ttl_ms : 0;
        bool router = NET_CTRL_HAS(a, NET_CTRL_EXT_FLAGS) && (a->flags & NET_CTRL_NEIGH_F_ROUTER);
        bool is_static = NET_CTRL_HAS(a, NET_CTRL_EXT_FLAGS) && (a->flags & NET_CTRL_NEIGH_F_STATIC);
        ndp_table_put_for_l2(l2->ifindex, a->address.ip, a->mac, ttl, router, is_static);
        return SOCK_OK;
    }
    return SOCK_ERR_INVAL;
}

static int32_t net_ctrl_neigh_del(const net_ctrl_attrs_t* a) {
    if (!NET_CTRL_HAS(a, NET_CTRL_EXT_ADDRESS)) return SOCK_ERR_INVAL;
    l2_interface_t* l2 = net_ctrl_l2_from_attrs(a);
    if (!l2) return SOCK_ERR_INVAL;
    if (a->address.ver == IP_VER4) {
        uint32_t ip = 0;
        memcpy(&ip, a->address.ip, sizeof(ip));
        if (!l2->arp_table) return SOCK_ERR_INVAL;
        return arp_table_delete_for_l2(l2->ifindex, ip) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    if (a->address.ver == IP_VER6) {
        if (!l2->nd_table) return SOCK_ERR_INVAL;
        return ndp_table_delete_for_l2(l2->ifindex, a->address.ip) ? SOCK_OK : SOCK_ERR_INVAL;
    }
    return SOCK_ERR_INVAL;
}

int32_t net_ctrl_dispatch(const void* req, uint32_t req_len, uint8_t** out, uint32_t* out_len) {
    if (!req || req_len < sizeof(NetCtrlMsg) || !out || !out_len) return SOCK_ERR_INVAL;
    *out = NULL;
    *out_len = 0;

    NetCtrlMsg in;
    memcpy(&in, req, sizeof(in));
    if (in.length != req_len || in.length < sizeof(NetCtrlMsg)) return SOCK_ERR_INVAL;
    if (!(in.flags & NET_CTRL_F_REQUEST)) return SOCK_ERR_INVAL;

    net_ctrl_attrs_t attrs;
    if (!net_ctrl_read_attrs(NET_CTRL_MSG_CONST_DATA((const NetCtrlMsg*)req), NET_CTRL_MSG_PAYLOAD_LEN(&in), &attrs)) return SOCK_ERR_INVAL;

    buffer b = buffer_create(256, buffer_can_grow);
    if (!b.buffer) return SOCK_ERR_SYS;
    NetCtrlMsg response = { in.object, in.op, NET_CTRL_F_RESPONSE, 0, sizeof(NetCtrlMsg), SOCK_OK };
    if (buffer_write_lim(&b, (const char*)&response, sizeof(response)) != sizeof(response)) {
        buffer_destroy(&b);
        return SOCK_ERR_SYS;
    }

    int32_t status;
    switch ((uint32_t)in.object) {
        case NET_CTRL_OBJ_LINK:
            switch ((uint32_t)in.op) {
                case NET_CTRL_OP_GET:
                    status = net_ctrl_link_dump(&attrs, &b) ? SOCK_OK : SOCK_ERR_SYS;
                    break;
                case NET_CTRL_OP_UPD:
                    status = net_ctrl_link_upd(&attrs);
                    break;
                default:
                    status = SOCK_ERR_INVAL;
                    break;
            }
            break;
        case NET_CTRL_OBJ_ADDR:
            switch ((uint32_t)in.op) {
                case NET_CTRL_OP_GET:
                    status = net_ctrl_addr_dump(&attrs, &b) ? SOCK_OK : SOCK_ERR_SYS;
                    break;
                case NET_CTRL_OP_ADD:
                    status = net_ctrl_addr_apply(&attrs, false);
                    break;
                case NET_CTRL_OP_UPD:
                    status = net_ctrl_addr_apply(&attrs, true);
                    break;
                case NET_CTRL_OP_DEL:
                    status = net_ctrl_addr_del(&attrs);
                    break;
                default:
                    status = SOCK_ERR_INVAL;
                    break;
            }
            break;
        case NET_CTRL_OBJ_ROUTE:
            switch ((uint32_t)in.op) {
                case NET_CTRL_OP_GET:
                    status = net_ctrl_route_dump(&attrs, &b) ? SOCK_OK : SOCK_ERR_SYS;
                    break;
                case NET_CTRL_OP_ADD:
                    status = net_ctrl_route_apply(&attrs, true);
                    break;
                case NET_CTRL_OP_UPD:
                    status = net_ctrl_route_apply(&attrs, false);
                    break;
                case NET_CTRL_OP_DEL:
                    status = net_ctrl_route_del(&attrs);
                    break;
                default:
                    status = SOCK_ERR_INVAL;
                    break;
            }
            break;
        case NET_CTRL_OBJ_NEIGH:
            switch ((uint32_t)in.op) {
                case NET_CTRL_OP_GET:
                    status = net_ctrl_neigh_dump(&attrs, &b) ? SOCK_OK : SOCK_ERR_SYS;
                    break;
                case NET_CTRL_OP_ADD:
                    status = net_ctrl_neigh_set(&attrs, true);
                    break;
                case NET_CTRL_OP_UPD:
                    status = net_ctrl_neigh_set(&attrs, false);
                    break;
                case NET_CTRL_OP_DEL:
                    status = net_ctrl_neigh_del(&attrs);
                    break;
                default:
                    status = SOCK_ERR_INVAL;
                    break;
            }
            break;
        default:
            status = SOCK_ERR_INVAL;
            break;
    }

    if (status != SOCK_OK) b.buffer_size = sizeof(NetCtrlMsg);
    NetCtrlMsg* hdr = (NetCtrlMsg*)b.buffer;
    hdr->status = status;
    hdr->length = b.buffer_size;
    *out = b.buffer;
    *out_len = b.buffer_size;
    return SOCK_OK;
}
