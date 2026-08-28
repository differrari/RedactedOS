#include "ip.h"
#include "types.h"
#include "console/kio.h"
#include "std/string.h"
#include "std/memory.h"
#include "alloc/allocate.h"
#include "net/net_ctrl.h"
#include "net/socket_types.h"
#include "networking/network.h"
#include "networking/transport_layer/net_ctrl.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/link_layer/link_utils.h"

static void print_help(void) {
    print("Usage:\t ip COMMAND [ARGS]");
    print("show or configure networking\n");
    print("Commands:");
    print("\t link\t interfaces");
    print("\t addr\t addresses");
    print("\t route\t routes");
    print("\t neigh\t neighbors\n");
    print("Args:");
    print("\t IFACE\t interface name");
    print("\t PREFIX\t ADDRESS/LENGTH");
    print("\t ADDRESS\t v4|v6 address");
    print("\t GATEWAY\t next hop");
    print("\t METRIC\t route metric\n");
    print("Options:");
    print("\t --help\t help");
}

static void req_init(uint8_t req[256], uint32_t* req_len, NetCtrlObject object, NetCtrlOp op) {
    memset(req, 0, 256);
    NetCtrlMsg* msg = (NetCtrlMsg*)req;
    msg->object = object;
    msg->op = op;
    msg->flags = NET_CTRL_F_REQUEST;
    *req_len = sizeof(NetCtrlMsg);
}

static bool req_attr(uint8_t req[256], uint32_t* req_len, NetCtrlExtAttr ext, const void* data, uint16_t len) {
    if (!data || *req_len + sizeof(NetCtrlAttr) + len > 256) return false;
    NetCtrlAttr attr = { .ext = ext, .length = len };
    memcpy(req + *req_len, &attr, sizeof(attr));
    *req_len += sizeof(attr);
    memcpy(req + *req_len, data, len);
    *req_len += len;
    return true;
}

static int32_t req_send(uint8_t req[256], uint32_t req_len, uint8_t** response) {
    if (response) *response = NULL;
    ((NetCtrlMsg*)req)->length = req_len;
    uint8_t* out = NULL;
    uint32_t out_len = 0;
    int32_t rc = net_ctrl_dispatch(req, req_len, &out, &out_len);
    if (rc != SOCK_OK || !out || out_len < sizeof(NetCtrlMsg)) {
        if (out) release(out);
        return rc == SOCK_OK ? SOCK_ERR_SYS : rc;
    }
    rc = ((NetCtrlMsg*)out)->status;
    if (rc != SOCK_OK || !response) release(out);
    else *response = out;
    return rc;
}

static bool parse_prefix(const char* s, net_l4_endpoint* ep, uint8_t* prefix) {
    char buf[96];
    if (!s || !ep || !prefix || strlen_max(s, sizeof(buf)) >= sizeof(buf)) return false;
    int32_t slash = str_has_char(s, 0, '/');
    if (slash <= 0) return false;
    strncpy(buf, s, sizeof(buf));
    buf[slash] = 0;

    uint32_t bits = 0, v4 = 0;
    uint8_t v6[16];
    if (!parse_uint32_dec_exact(buf + slash + 1, &bits)) return false;
    if (ipv4_parse(buf, &v4)) make_ep(&v4, 0, IP_VER4, ep);
    else if (ipv6_parse(buf, v6)) make_ep(v6, 0, IP_VER6, ep);
    else return false;
    if ((ep->ver == IP_VER4 && bits > 32) || (ep->ver == IP_VER6 && bits > 128)) return false;
    *prefix = (uint8_t)bits;
    return true;
}

static int show_link(void) {
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OBJ_LINK, NET_CTRL_OP_GET);
    uint8_t* response = NULL;
    int32_t rc = req_send(req, req_len, &response);
    if (rc != SOCK_OK) return 1;
    NetCtrlMsg* msg = (NetCtrlMsg*)response;
    uint32_t count = NET_CTRL_MSG_PAYLOAD_LEN(msg) / sizeof(NetCtrlLinkInfo);
    NetCtrlLinkInfo* links = NET_CTRL_MSG_DATA(msg);
    for (uint32_t i = 0; i < count; i++) print("%u: %s: %s mtu %u metric %u", links[i].ifindex, links[i].name, links[i].up ? "UP" : "DOWN", links[i].mtu, links[i].metric);
    release(response);
    return 0;
}

static int set_link(int argc, char* argv[]) {
    if (argc != 5) return 2;
    uint8_t state;
    if (strcmp_case(argv[4], "up", true) == 0) state = 1;
    else if (strcmp_case(argv[4], "down", true) == 0) state = 0;
    else return 2;
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OBJ_LINK, NET_CTRL_OP_UPD);
    size_t ifname_len = strlen_max(argv[3], 17);
    if (!ifname_len || ifname_len > 16 || !req_attr(req, &req_len, NET_CTRL_EXT_IFNAME, argv[3], (uint16_t)ifname_len) || !req_attr(req, &req_len, NET_CTRL_EXT_STATE, &state, sizeof(state))) return 1;
    return req_send(req, req_len, NULL) == SOCK_OK ? 0 : 1;
}

static int show_addr(void) {
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OBJ_ADDR, NET_CTRL_OP_GET);
    uint8_t* response = NULL;
    if (req_send(req, req_len, &response) != SOCK_OK) return 1;
    NetCtrlMsg* msg = (NetCtrlMsg*)response;
    uint32_t count = NET_CTRL_MSG_PAYLOAD_LEN(msg) / sizeof(NetCtrlAddrInfo);
    NetCtrlAddrInfo* addrs = NET_CTRL_MSG_DATA(msg);
    for (uint32_t i = 0; i < count; i++) {
        char addr[64];
        net_ep_split(&addrs[i].prefix.address, addr, sizeof(addr), NULL, NULL);
        const char* name = network_get_ifname(addrs[i].prefix.ifindex);
        print("%s %s/%u dev %s", addrs[i].prefix.address.ver == IP_VER6 ? "inet6" : "inet", addr, addrs[i].prefix.prefix_len, name ? name : "?");
    }
    release(response);
    return 0;
}

static int addr_add(int argc, char* argv[]) {
    if (argc != 6 || strcmp_case(argv[4], "dev", true) != 0) return 2;
    net_l4_endpoint ep;
    uint8_t prefix;
    if (!parse_prefix(argv[3], &ep, &prefix)) return 2;
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OBJ_ADDR, NET_CTRL_OP_ADD); //b
    size_t ifname_len = strlen_max(argv[5], 17);
    if (!ifname_len || ifname_len > 16 || !req_attr(req, &req_len, NET_CTRL_EXT_IFNAME, argv[5], (uint16_t)ifname_len) ||
        !req_attr(req, &req_len, NET_CTRL_EXT_ADDRESS, &ep, sizeof(ep)) ||
        !req_attr(req, &req_len, NET_CTRL_EXT_PREFIX_LEN, &prefix, sizeof(prefix))) return 1;
    return req_send(req, req_len, NULL) == SOCK_OK ? 0 : 1;
}

static int addr_del(int argc, char* argv[]) {
    if (argc != 6 || strcmp_case(argv[4], "dev", true) != 0) return 2;
    net_l4_endpoint ep = {0};
    uint32_t v4 = 0;
    uint8_t v6[16];
    if (ipv4_parse(argv[3], &v4)) make_ep(&v4, 0, IP_VER4, &ep);
    else if (ipv6_parse(argv[3], v6)) make_ep(v6, 0, IP_VER6, &ep);
    else return 2;

    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OBJ_ADDR, NET_CTRL_OP_GET);
    size_t ifname_len = strlen_max(argv[5], 17);
    if (!ifname_len || ifname_len > 16 || !req_attr(req, &req_len, NET_CTRL_EXT_IFNAME, argv[5], (uint16_t)ifname_len) || !req_attr(req, &req_len, NET_CTRL_EXT_ADDRESS, &ep, sizeof(ep))) return 1;
    uint8_t* response = NULL;
    if (req_send(req, req_len, &response) != SOCK_OK) return 1;
    NetCtrlMsg* msg = (NetCtrlMsg*)response;
    uint32_t count = NET_CTRL_MSG_PAYLOAD_LEN(msg)/sizeof(NetCtrlAddrInfo);
    NetCtrlAddrInfo* addrs = NET_CTRL_MSG_DATA(msg);
    uint8_t l3_id = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t bytes = ep.ver == IP_VER4 ? 4 : 16;
        if (addrs[i].prefix.address.ver == ep.ver && memcmp(addrs[i].prefix.address.ip, ep.ip, bytes) == 0) {
            l3_id = addrs[i].prefix.l3_id;
            break;
        }
    }
    release(response);
    if (!l3_id) return 1;
    req_init(req, &req_len, NET_CTRL_OBJ_ADDR, NET_CTRL_OP_DEL);
    if (!req_attr(req, &req_len, NET_CTRL_EXT_L3_ID, &l3_id, sizeof(l3_id))) return 1;
    return req_send(req, req_len, NULL) == SOCK_OK ? 0 : 1;
}

static int show_route(void) {
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OBJ_ROUTE, NET_CTRL_OP_GET);
    uint8_t* response = NULL;
    if (req_send(req, req_len, &response) != SOCK_OK) return 1;
    NetCtrlMsg* msg = (NetCtrlMsg*)response;
    uint32_t count = NET_CTRL_MSG_PAYLOAD_LEN(msg) / sizeof(NetCtrlRouteInfo);
    NetCtrlRouteInfo* routes = NET_CTRL_MSG_DATA(msg);
    for (uint32_t i = 0; i < count; i++) {
        char dst[64], gw[64];
        net_ep_split(&routes[i].prefix.address, dst, sizeof(dst), NULL, NULL);
        net_ep_split(&routes[i].prefix.gateway, gw, sizeof(gw), NULL, NULL);
        bool def = routes[i].prefix.prefix_len == 0;
        uint32_t gateway_v4 = 0;
        memcpy(&gateway_v4, routes[i].prefix.gateway.ip, sizeof(gateway_v4));
        bool has_gw = routes[i].prefix.gateway.ver == IP_VER4 ? !ipv4_is_unspecified(gateway_v4) : !ipv6_is_unspecified(routes[i].prefix.gateway.ip);
        const char* name = network_get_ifname(routes[i].prefix.ifindex);
        if (!name) name = "?";
        if (def && has_gw) print("default via %s dev %s metric %u", gw, name, routes[i].metric);
        else if (def) print("default dev %s metric %u", name, routes[i].metric);
        else if (has_gw) print("%s/%u via %s dev %s metric %u", dst, routes[i].prefix.prefix_len, gw, name, routes[i].metric);
        else print("%s/%u dev %s metric %u", dst, routes[i].prefix.prefix_len, name, routes[i].metric);
    }
    release(response);
    return 0;
}

static int route_change(int argc, char* argv[], bool add) {
    if (argc < 6) return 2;
    net_l4_endpoint network = {0}, gateway = {0};
    uint8_t prefix = 0;
    bool has_gateway = false;
    const char* dev = NULL;
    uint16_t metric = 0;

    int i = 3;
    bool def = strcmp_case(argv[i], "default", true) == 0;
    if (def) i++;
    else if (!parse_prefix(argv[i++], &network, &prefix)) return 2;

    while (i < argc) {
        if (strcmp_case(argv[i], "via", true) == 0) {
            if (++i >= argc || has_gateway) return 2;
            uint32_t v4 = 0;
            uint8_t v6[16];
            if (ipv4_parse(argv[i], &v4)) make_ep(&v4, 0, IP_VER4, &gateway);
            else if (ipv6_parse(argv[i], v6)) make_ep(v6, 0, IP_VER6, &gateway);
            else return 2;
            i++;
            has_gateway = true;
        } else if (strcmp_case(argv[i], "dev", true) == 0) {
            if (++i >= argc || dev) return 2;
            dev = argv[i++];
        } else if (add && strcmp_case(argv[i], "metric", true) == 0) {
            uint32_t value = 0;
            if (++i >= argc || !parse_uint32_dec_exact(argv[i++], &value) || value > UINT16_MAX) return 2;
            metric = (uint16_t)value;
        } else return 2;
    }
    if (!dev || (def && !has_gateway) || (!add && !def && has_gateway)) return 2;
    if (def) network.ver = gateway.ver;
    if (has_gateway && gateway.ver != network.ver) return 2;

    uint8_t l3_id = 0;
    bool linklocal = network.ver == IP_VER6 && !def && ipv6_is_linklocal(network.ip);
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OBJ_ADDR, NET_CTRL_OP_GET);
    size_t ifname_len = strlen_max(dev, 17);
    if (!ifname_len || ifname_len > 16 || !req_attr(req, &req_len, NET_CTRL_EXT_IFNAME, dev, (uint16_t)ifname_len)) return 1;
    uint8_t* response = NULL;
    if (req_send(req, req_len, &response) != SOCK_OK) return 1;
    NetCtrlMsg* msg = (NetCtrlMsg*)response;
    uint32_t count = NET_CTRL_MSG_PAYLOAD_LEN(msg) / sizeof(NetCtrlAddrInfo);
    NetCtrlAddrInfo* addrs = NET_CTRL_MSG_DATA(msg);
    for (uint32_t j = 0; j < count; j++) {
        if (addrs[j].prefix.address.ver != network.ver) continue;
        if (network.ver == IP_VER6 && ipv6_is_linklocal(addrs[j].prefix.address.ip) != linklocal) continue;
        l3_id = addrs[j].prefix.l3_id;
        break;
    }
    release(response);
    if (!l3_id) return 1;

    req_init(req, &req_len, NET_CTRL_OBJ_ROUTE, add ? NET_CTRL_OP_ADD : NET_CTRL_OP_DEL);
    if (!req_attr(req, &req_len, NET_CTRL_EXT_L3_ID, &l3_id, sizeof(l3_id)) ||
        !req_attr(req, &req_len, NET_CTRL_EXT_ADDRESS, &network, sizeof(network)) ||
        !req_attr(req, &req_len, NET_CTRL_EXT_PREFIX_LEN, &prefix, sizeof(prefix))) return 1;
    if (has_gateway && !req_attr(req, &req_len, NET_CTRL_EXT_GATEWAY, &gateway, sizeof(gateway))) return 1;
    if (add && metric && !req_attr(req, &req_len, NET_CTRL_EXT_METRIC, &metric, sizeof(metric))) return 1;
    return req_send(req, req_len, NULL) == SOCK_OK ? 0 : 1;
}

static int show_neigh(void) {
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OBJ_NEIGH, NET_CTRL_OP_GET);
    uint8_t* response = NULL;
    if (req_send(req, req_len, &response) != SOCK_OK) return 1;
    NetCtrlMsg* msg = (NetCtrlMsg*)response;
    uint32_t count = NET_CTRL_MSG_PAYLOAD_LEN(msg) / sizeof(NetCtrlNeighInfo);
    NetCtrlNeighInfo* entries = NET_CTRL_MSG_DATA(msg);
    for (uint32_t i = 0; i < count; i++) {
        char addr[64], mac[18];
        net_ep_split(&entries[i].address, addr, sizeof(addr), NULL, NULL);
        mac_to_string(entries[i].mac, mac);
        const char* name = network_get_ifname(entries[i].ifindex);
        print("%s dev %s lladdr %s%s", addr, name ? name : "?", mac, (entries[i].flags & NET_CTRL_NEIGH_F_STATIC) ? " static" : "");
    }
    release(response);
    return 0;
}

int run_ip(int argc, char* argv[]) {
    if (argc == 2 && strcmp_case(argv[1], "--help", true) == 0) {
        print_help();
        return 0;
    }

    int rc = 2;
    if (argc == 2 && strcmp_case(argv[1], "link", true) == 0) rc = show_link();
    else if (argc >= 3 && strcmp_case(argv[1], "link", true) == 0 && strcmp_case(argv[2], "set", true) == 0) rc = set_link(argc, argv);
    else if (argc == 2 && strcmp_case(argv[1], "addr", true) == 0) rc = show_addr();
    else if (argc >= 3 && strcmp_case(argv[1], "addr", true) == 0 && strcmp_case(argv[2], "add", true) == 0) rc = addr_add(argc, argv);
    else if (argc >= 3 && strcmp_case(argv[1], "addr", true) == 0 && strcmp_case(argv[2], "del", true) == 0) rc = addr_del(argc, argv);
    else if (argc == 2 && strcmp_case(argv[1], "route", true) == 0) rc = show_route();
    else if (argc >= 3 && strcmp_case(argv[1], "route", true) == 0 && strcmp_case(argv[2], "add", true) == 0) rc = route_change(argc, argv, true);
    else if (argc >= 3 && strcmp_case(argv[1], "route", true) == 0 && strcmp_case(argv[2], "del", true) == 0) rc = route_change(argc, argv, false);
    else if (argc == 2 && strcmp_case(argv[1], "neigh", true) == 0) rc = show_neigh();

    if (rc != 2) return rc;
    print_help();
    return 2;
}
