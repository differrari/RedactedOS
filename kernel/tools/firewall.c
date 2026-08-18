#include "firewall.h"
#include "types.h"
#include "console/kio.h"
#include "std/string.h"
#include "std/memory.h"
#include "alloc/allocate.h"
#include "net/net_ctrl.h"
#include "net/socket_types.h"
#include "networking/transport_layer/net_ctrl.h"
#include "networking/transport_layer/trans_utils.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"

static void req_init(uint8_t req[256], uint32_t* req_len, NetCtrlOp op) {
    memset(req, 0, 256);
    NetCtrlMsg* msg = (NetCtrlMsg*)req;
    msg->object = NET_CTRL_OBJ_FIREWALL;
    msg->op = op;
    msg->flags = NET_CTRL_F_REQUEST;
    *req_len = sizeof(NetCtrlMsg);
}

static bool req_attr(uint8_t req[256], uint32_t* req_len, NetCtrlExtAttr ext, const void* data, uint16_t len) {
    if (!data || *req_len + sizeof(NetCtrlAttr) + len > 256) return false;
    NetCtrlAttr attr = {.ext = ext, .length = len};
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

static int show_firewall(bool list) {
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OP_GET);
    uint8_t* response = NULL;
    int32_t rc = req_send(req, req_len, &response);
    if (rc != SOCK_OK) {
        print("firewall: request failed (%i)", rc);
        return 1;
    }

    NetCtrlMsg* msg = (NetCtrlMsg*)response;
    uint32_t len = NET_CTRL_MSG_PAYLOAD_LEN(msg);
    if (len < sizeof(NetCtrlFirewallState)) {
        release(response);
        return 1;
    }
    NetCtrlFirewallState* state = NET_CTRL_MSG_DATA(msg);
    print("firewall: %s", state->enabled ? "on" : "off");
    print("default: in %s, out %s", state->default_in == NET_CTRL_FIREWALL_ALLOW ? "allow" : "deny", state->default_out == NET_CTRL_FIREWALL_ALLOW ? "allow" : "deny");

    if (list) {
        uint32_t available = (len - sizeof(*state)) / sizeof(NetCtrlFirewallRule);
        uint32_t count = state->rule_count < available ? state->rule_count : available;
        NetCtrlFirewallRule* rules = (NetCtrlFirewallRule*)((uint8_t*)state + sizeof(*state));
        if (!count) print("no rules");
        for (uint32_t i = 0; i < count; i++) {
            char address[96], port[24];
            if (!rules[i].ip_version) string_format_buf(address, sizeof(address), "any");
            else {
                net_l4_endpoint ep;
                make_ep(rules[i].address, 0, (ip_version_t)rules[i].ip_version, &ep);
                char value[64];
                net_ep_split(&ep, value, sizeof(value), NULL, NULL);
                string_format_buf(address, sizeof(address), "%s/%u", value, rules[i].prefix_len);
            }
            if (!rules[i].port_from) string_format_buf(port, sizeof(port), "any");
            else if (rules[i].port_from == rules[i].port_to) string_format_buf(port, sizeof(port), "%u", rules[i].port_from);
            else string_format_buf(port, sizeof(port), "%u-%u", rules[i].port_from, rules[i].port_to);
            const char* direction = rules[i].direction == NET_CTRL_FIREWALL_IN ? "in" : "out";
            const char* protocol = rules[i].protocol == PROTO_TCP ? "tcp" : rules[i].protocol == PROTO_UDP ? "udp" : "any";
            print("%u %s %s %s %s port %s", rules[i].id, rules[i].action == NET_CTRL_FIREWALL_ALLOW ? "allow" : "deny", direction, protocol, address, port);
        }
    }
    release(response);
    return 0;
}

static int update(uint8_t state, uint8_t direction, uint8_t action) {
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OP_UPD);
    if (state != UINT8_MAX && !req_attr(req, &req_len, NET_CTRL_EXT_STATE, &state, sizeof(state))) return 1;
    if (direction && (!req_attr(req, &req_len, NET_CTRL_EXT_DIRECTION, &direction, sizeof(direction)) || !req_attr(req, &req_len, NET_CTRL_EXT_ACTION, &action, sizeof(action)))) return 1;
    int32_t rc = req_send(req, req_len, NULL);
    if (rc != SOCK_OK) print("firewall: update failed (%i)", rc);
    return rc == SOCK_OK ? 0 : 1;
}

static int delete_rule(const char* value) {
    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OP_DEL);
    if (value) {
        uint32_t id = 0;
        if (!parse_uint32_dec_exact(value, &id) || !id || !req_attr(req, &req_len, NET_CTRL_EXT_RULE_ID, &id, sizeof(id))) return 2;
    }
    int32_t rc = req_send(req, req_len, NULL);
    if (rc != SOCK_OK) print("firewall: delete failed (%i)", rc);
    return rc == SOCK_OK ? 0 : 1;
}

static int add_rule(int argc, char* argv[]) {
    if (argc < 4) return 2;
    uint8_t action = 0;
    if (strcmp_case(argv[1], "allow", true) == 0) action = NET_CTRL_FIREWALL_ALLOW;
    else if (strcmp_case(argv[1], "deny", true) == 0) action = NET_CTRL_FIREWALL_DENY;
    else return 2;

    uint8_t direction = 0;
    if (strcmp_case(argv[2], "in", true) == 0) direction = NET_CTRL_FIREWALL_IN;
    else if (strcmp_case(argv[2], "out", true) == 0) direction = NET_CTRL_FIREWALL_OUT;
    else return 2;

    uint8_t protocol = PROTO_NONE;
    if (strcmp_case(argv[3], "tcp", true) == 0) protocol = PROTO_TCP;
    else if (strcmp_case(argv[3], "udp", true) == 0) protocol = PROTO_UDP;
    else if (strcmp_case(argv[3], "any", true) != 0) return 2;

    net_l4_endpoint address = {0};
    uint8_t prefix = 0;
    uint16_t port_from = 0, port_to = 0;
    bool address_seen = false, has_address = false, has_port = false;
    for (int i = 4; i < argc; i++) {
        if (strcmp_case(argv[i], direction == NET_CTRL_FIREWALL_IN ? "from" : "to", true) == 0) {
            if (++i >= argc || address_seen) return 2;
            address_seen = true;
            has_address = strcmp_case(argv[i], "any", true) != 0;
            if (has_address) {
                const char* value = argv[i];
                int32_t slash = str_has_char(value, 0, '/');
                uint32_t bits = UINT32_MAX, v4 = 0;
                uint8_t v6[16];
                char ip[80];
                if (slash >= 0) {
                    if (!slash || strlen_max(value, sizeof(ip)) >= sizeof(ip) || !parse_uint32_dec_exact(value + slash + 1, &bits)) return 2;
                    strncpy(ip, value, sizeof(ip));
                    ip[slash] = 0;
                    value = ip;
                }
                if (ipv4_parse(value, &v4)) {
                    make_ep(&v4, 0, IP_VER4, &address);
                    if (bits == UINT32_MAX) bits = 32;
                    if (bits > 32) return 2;
                } else if (ipv6_parse(value, v6)) {
                    make_ep(v6, 0, IP_VER6, &address);
                    if (bits == UINT32_MAX) bits = 128;
                    if (bits > 128) return 2;
                } else return 2;
                prefix = (uint8_t)bits;
            }
        } else if (strcmp_case(argv[i], "port", true) == 0) {
            if (++i >= argc || has_port) return 2;
            const char* value = argv[i];
            int32_t dash = str_has_char(value, 0, '-');
            uint32_t first = 0, last = 0;
            if (dash >= 0) {
                char range[24];
                if (!dash || strlen_max(value, sizeof(range)) >= sizeof(range)) return 2;
                strncpy(range, value, sizeof(range));
                range[dash] = 0;
                if (!parse_uint32_dec_exact(range, &first) || !parse_uint32_dec_exact(range + dash + 1, &last)) return 2;
            } else {
                if (!parse_uint32_dec_exact(value, &first)) return 2;
                last = first;
            }
            if (!first || !last || first > UINT16_MAX || last > UINT16_MAX || first > last) return 2;
            port_from = (uint16_t)first;
            port_to = (uint16_t)last;
            has_port = true;
        } else return 2;
    }

    uint8_t req[256];
    uint32_t req_len;
    req_init(req, &req_len, NET_CTRL_OP_ADD);
    if (!req_attr(req, &req_len, NET_CTRL_EXT_ACTION, &action, sizeof(action)) || !req_attr(req, &req_len, NET_CTRL_EXT_DIRECTION, &direction, sizeof(direction)) || 
        !req_attr(req, &req_len, NET_CTRL_EXT_PROTOCOL, &protocol, sizeof(protocol))) return 1;
    if (has_address && (!req_attr(req, &req_len, NET_CTRL_EXT_ADDRESS, &address, sizeof(address)) || !req_attr(req, &req_len, NET_CTRL_EXT_PREFIX_LEN, &prefix, sizeof(prefix)))) return 1;
    if (has_port && (!req_attr(req, &req_len, NET_CTRL_EXT_PORT_FROM, &port_from, sizeof(port_from)) || !req_attr(req, &req_len, NET_CTRL_EXT_PORT_TO, &port_to, sizeof(port_to)))) return 1;
    int32_t rc = req_send(req, req_len, NULL);
    if (rc != SOCK_OK) print("firewall: add failed (%i)", rc);
    return rc == SOCK_OK ? 0 : 1;
}

int run_firewall(int argc, char* argv[]) {
    int rc = 2;
    if (argc == 1 || (argc == 2 && strcmp_case(argv[1], "list", true) == 0)) rc = show_firewall(true);
    else if (argc == 2 && strcmp_case(argv[1], "status", true) == 0) rc = show_firewall(false);
    else if (argc == 2 && strcmp_case(argv[1], "on", true) == 0) rc = update(1, 0, 0);
    else if (argc == 2 && strcmp_case(argv[1], "off", true) == 0) rc = update(0, 0, 0);
    else if (argc == 2 && strcmp_case(argv[1], "clear", true) == 0) rc = delete_rule(NULL);
    else if (argc == 3 && strcmp_case(argv[1], "delete", true) == 0) rc = delete_rule(argv[2]);
    else if (argc == 4 && strcmp_case(argv[1], "default", true) == 0) {
        uint8_t direction = 0;
        if (strcmp_case(argv[2], "in", true) == 0) direction = NET_CTRL_FIREWALL_IN;
        else if (strcmp_case(argv[2], "out", true) == 0) direction = NET_CTRL_FIREWALL_OUT;

        uint8_t action = 0;
        if (strcmp_case(argv[3], "allow", true) == 0) action = NET_CTRL_FIREWALL_ALLOW;
        else if (strcmp_case(argv[3], "deny", true) == 0) action = NET_CTRL_FIREWALL_DENY;

        if (direction && action) rc = update(UINT8_MAX, direction, action);
    } else if (argc == 2 && strcmp_case(argv[1], "reset", true) == 0) {
        if (delete_rule(NULL) || update(UINT8_MAX, NET_CTRL_FIREWALL_IN, NET_CTRL_FIREWALL_DENY) || update(UINT8_MAX, NET_CTRL_FIREWALL_OUT, NET_CTRL_FIREWALL_ALLOW)) rc = 1;
        else rc = update(1, 0, 0);
    } else if (argc >= 4 && (strcmp_case(argv[1], "allow", true) == 0 || strcmp_case(argv[1], "deny", true) == 0)) {
        rc = add_rule(argc, argv);
    }

    if (rc != 2) return rc;
    print("usage: firewall [status|list|on|off|clear|reset]");
    print("       firewall default in|out allow|deny");
    print("       firewall delete id");
    print("       firewall allow|deny in tcp|udp|any [from cidr|any] [port n|a-b]");
    print("       firewall allow|deny out tcp|udp|any [to cidr|any] [port n|a-b]");
    return 2;
}
