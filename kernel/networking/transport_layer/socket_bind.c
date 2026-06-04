#include "socket_bind.h"
#include "exceptions/irq.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "random/random.h"
#include "std/memory.h"

#define SOCKET_PORT_MIN_EPHEMERAL 49152u
#define SOCKET_PORT_MAX_EPHEMERAL 65535u

typedef struct socket_bind_entry {
    bool used;
    protocol_t protocol;
    uint16_t port;
    SockBindSpec spec;
    ksocket_t* socket;
} socket_bind_entry_t;

static socket_bind_entry_t bind_entries[SOCKET_BIND_MAX];

static bool socket_bind_l3_valid(ip_version_t ver, uint8_t l3_id) {
    if (!l3_id) return false;

    if (ver == IP_VER4) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
        return v4 && v4->l2 && v4->l2->is_up && v4->mode != IPV4_CFG_DISABLED;
    }

    if (ver == IP_VER6) {
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
        return v6 && v6->l2 && v6->l2->is_up && v6->cfg != IPV6_CFG_DISABLE && v6->dad_state == IPV6_DAD_OK;
    }

    return false;
}

uint32_t socket_bind_l3_list(const SockBindSpec* spec, ip_version_t ver, uint8_t* out, uint32_t cap) {
    if (!spec || !out || !cap || (ver != IP_VER4 && ver != IP_VER6)) return 0;

    uint32_t count = 0;
    SockBindSpec normal = *spec;
    if (normal.kind == BIND_L3 && normal.l3_id == 0 && normal.ifindex == 0 && normal.ver == 0 && ipv6_is_unspecified(normal.ip)) normal.kind = BIND_ANY;

    if (normal.kind == BIND_ANY4 && ver != IP_VER4) return 0;
    if (normal.kind == BIND_ANY6 && ver != IP_VER6) return 0;
    if (normal.ver && normal.ver != ver && normal.kind != BIND_ANY && normal.kind != BIND_ANY4 && normal.kind != BIND_ANY6) return 0;

    if (normal.kind == BIND_L3) {
        if (socket_bind_l3_valid(ver, normal.l3_id)) out[count++] = normal.l3_id;
        return count;
    }

    if (normal.kind == BIND_IP) {
        if (ver == IP_VER4) {
            uint32_t ip = 0;
            memcpy(&ip, normal.ip, 4);
            if (!ipv4_is_unspecified(ip) && !ipv4_is_multicast(ip)) {
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(ip);
                if (v4 && socket_bind_l3_valid(IP_VER4, v4->l3_id)) out[count++] = v4->l3_id;
                return count;
            }
        } else {
            if (!ipv6_is_unspecified(normal.ip) && !ipv6_is_multicast(normal.ip)) {
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_ip(normal.ip);
                if (v6 && socket_bind_l3_valid(IP_VER6, v6->l3_id)) out[count++] = v6->l3_id;
                return count;
            }
        }
    }

    for (uint8_t i = 0, n = l2_interface_count(); i < n && count < cap; ++i) {
        l2_interface_t* l2 = l2_interface_at(i);
        if (!l2 || !l2->is_up) continue;
        if (normal.kind == BIND_L2 && l2->ifindex != normal.ifindex) continue;
        if (normal.kind != BIND_ANY && normal.kind != BIND_ANY4 && normal.kind != BIND_ANY6 && normal.kind != BIND_IP && normal.kind != BIND_L2) continue;

        if (ver == IP_VER4) {
            for (int s = 0; s < MAX_IPV4_PER_INTERFACE && count < cap; ++s) {
                l3_ipv4_interface_t* v4 = l2->l3_v4[s];
                if (!v4 || !socket_bind_l3_valid(IP_VER4, v4->l3_id)) continue;
                out[count++] = v4->l3_id;
            }
        } else {
            for (int s = 0; s < MAX_IPV6_PER_INTERFACE && count < cap; ++s) {
                l3_ipv6_interface_t* v6 = l2->l3_v6[s];
                if (!v6 || !socket_bind_l3_valid(IP_VER6, v6->l3_id)) continue;
                out[count++] = v6->l3_id;
            }
        }
    }

    return count;
}

bool socket_bind_insert(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, uint16_t port) {
    if (!socket || !spec || !port) return false;
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return false;

    SockBindSpec normal = *spec;
    if (normal.kind == BIND_L3 && normal.l3_id == 0 && normal.ifindex == 0 && normal.ver == 0 && ipv6_is_unspecified(normal.ip)) normal.kind = BIND_ANY;

    irq_flags_t irq = irq_save_disable(); //TODO lock

    for (uint32_t i = 0; i < SOCKET_BIND_MAX; ++i) {
        socket_bind_entry_t* e = &bind_entries[i];
        if (!e->used) continue;
        if (e->protocol != protocol || e->port != port) continue;

        ip_version_t a_family = normal.kind == BIND_ANY4 ? IP_VER4 : (normal.kind == BIND_ANY6 ? IP_VER6 : normal.ver);
        ip_version_t b_family = e->spec.kind == BIND_ANY4 ? IP_VER4 : (e->spec.kind == BIND_ANY6 ? IP_VER6 : e->spec.ver);
        if (a_family && b_family && a_family != b_family) continue;

        bool overlap = true;
        if (normal.kind == BIND_IP && e->spec.kind == BIND_IP && a_family && b_family) {
            bool a_unspec = true;
            bool b_unspec = true;
            if (a_family == IP_VER4) {
                uint32_t av = 0;
                uint32_t bv = 0;
                memcpy(&av, normal.ip, 4);
                memcpy(&bv, e->spec.ip, 4);
                a_unspec = ipv4_is_unspecified(av);
                b_unspec = ipv4_is_unspecified(bv);
                overlap = a_unspec || b_unspec || av == bv;
            } else if (a_family == IP_VER6) {
                a_unspec = ipv6_is_unspecified(normal.ip);
                b_unspec = ipv6_is_unspecified(e->spec.ip);
                overlap = a_unspec || b_unspec || memcmp(normal.ip, e->spec.ip, 16) == 0;
            }
        } else if (normal.kind == BIND_L3 && e->spec.kind == BIND_L3) overlap = normal.l3_id == e->spec.l3_id;
        else if (normal.kind == BIND_L2 && e->spec.kind == BIND_L2) overlap = normal.ifindex == e->spec.ifindex;

        if (overlap) {
            irq_restore(irq);
            return false;
        }
    }

    for (uint32_t i = 0; i < SOCKET_BIND_MAX; ++i) {
        socket_bind_entry_t* e = &bind_entries[i];
        if (e->used) continue;
        e->used = true;
        e->protocol = protocol;
        e->port = port;
        e->spec = normal;
        e->socket = socket;
        socket_core_ref(socket);
        irq_restore(irq);
        return true;
    }

    irq_restore(irq);
    return false;
}

void socket_bind_remove_socket(ksocket_t* socket) {
    if (!socket) return;

    ksocket_t* drop[SOCKET_BIND_MAX];
    uint32_t drop_count = 0;

    irq_flags_t irq = irq_save_disable(); //TODO lock
    for (uint32_t i = 0; i < SOCKET_BIND_MAX; ++i) {
        socket_bind_entry_t* e = &bind_entries[i];
        if (!e->used || e->socket != socket) continue;
        drop[drop_count++] = e->socket;
        memset(e, 0, sizeof(*e));
    }
    irq_restore(irq);

    for (uint32_t i = 0; i < drop_count; ++i) socket_core_put(drop[i]);
}

bool socket_bind_port_busy(protocol_t protocol, ip_version_t ipver, uint8_t l3_id, uint16_t port) {
    if (!port) return true;
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return true;

    irq_flags_t irq = irq_save_disable(); //TODO lock
    for (uint32_t i = 0; i < SOCKET_BIND_MAX; ++i) {
        socket_bind_entry_t* e = &bind_entries[i];
        if (!e->used) continue;
        if (e->protocol != protocol || e->port != port) continue;

        ip_version_t family = e->spec.kind == BIND_ANY4 ? IP_VER4 : (e->spec.kind == BIND_ANY6 ? IP_VER6 : e->spec.ver);
        if (family && family != ipver) continue;
        if (e->spec.kind == BIND_L3 && e->spec.l3_id && e->spec.l3_id != l3_id) continue;

        irq_restore(irq);
        return true;
    }
    irq_restore(irq);
    return false;
}

int socket_bind_alloc_ephemeral_l3(ksocket_t* socket, protocol_t protocol, uint8_t l3_id, uint16_t pid) {
    if (!socket || !l3_id) return -1;
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return -1;

    ip_version_t ver = l3_is_v6_from_id(l3_id) ? IP_VER6 : IP_VER4;

    rng_t rng;
    rng_init_random(&rng);
    uint32_t seed = rng_next32(&rng);

    uint32_t minp = SOCKET_PORT_MIN_EPHEMERAL;
    uint32_t maxp = SOCKET_PORT_MAX_EPHEMERAL;
    uint32_t range = maxp - minp + 1u;
    uint32_t first = minp + (seed % range);

    SockBindSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = BIND_L3;
    spec.ver = ver;
    spec.l3_id = l3_id;

    for (uint32_t i = 0; i < range; ++i) {
        uint16_t port = (uint16_t)(minp + ((first - minp + i) % range));
        if (socket_bind_port_busy(protocol, ver, l3_id, port)) continue;
        if (socket_bind_insert(socket, protocol, &spec, port)) return port;
    }

    return -1;
}

uint32_t socket_bind_collect(protocol_t protocol, ip_version_t ipver, uint8_t l3_id, uint8_t ifindex, const void* dst_ip_addr, uint16_t dst_port, ksocket_t** out, uint32_t out_cap) {
    if (!out || !out_cap || !dst_ip_addr) return 0;
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return 0;

    uint32_t count = 0;
    irq_flags_t irq = irq_save_disable(); //TODO lock
    for (uint32_t i = 0; i < SOCKET_BIND_MAX && count < out_cap; ++i) {
        socket_bind_entry_t* e = &bind_entries[i];
        if (!e->used || e->protocol != protocol || e->port != dst_port || !e->socket) continue;

        ip_version_t family = e->spec.kind == BIND_ANY4 ? IP_VER4 : (e->spec.kind == BIND_ANY6 ? IP_VER6 : e->spec.ver);
        if (family && family != ipver) continue;

        if (e->spec.kind == BIND_L3 && e->spec.l3_id != l3_id) continue;
        if (e->spec.kind == BIND_L2 && e->spec.ifindex != ifindex) continue;

        if (e->spec.kind == BIND_IP) {
            if (ipver == IP_VER4) {
                uint32_t want = 0;
                uint32_t got = 0;
                memcpy(&want, e->spec.ip, 4);
                memcpy(&got, dst_ip_addr, 4);
                if (!ipv4_is_unspecified(want) && want != got) continue;
            } else if (ipver == IP_VER6) {
                if (!ipv6_is_unspecified(e->spec.ip) && memcmp(e->spec.ip, dst_ip_addr, 16) != 0) continue;
            } else continue;
        } else if (e->spec.kind != BIND_ANY && e->spec.kind != BIND_ANY4 && e->spec.kind != BIND_ANY6 && e->spec.kind != BIND_L3 && e->spec.kind != BIND_L2) continue;

        if (socket_core_is_closing(e->socket)) continue;
        socket_core_ref(e->socket);
        out[count++] = e->socket;
    }
    irq_restore(irq);
    return count;
}