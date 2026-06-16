#include "socket_bind.h"
#include "exceptions/irq.h"
#include "networking/interface_manager.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "networking/internet_layer/ipv4_route.h"
#include "networking/internet_layer/ipv6_route.h"
#include "random/random.h"
#include "std/memory.h"
#include "data/struct/hashmap.h"

#define SOCKET_PORT_MIN_EPHEMERAL 49152u
#define SOCKET_PORT_MAX_EPHEMERAL 65535u
#define SOCKET_BIND_LIST_END 0xFFFF

typedef struct socket_bind_entry {
    bool used;
    protocol_t protocol;
    uint16_t port;
    uint16_t next;
    uint32_t generation;
    SockBindSpec spec;
    ksocket_t* socket;
} socket_bind_entry_t;

static socket_bind_entry_t bind_entries[SOCKET_BIND_MAX];
static hash_map_t* bind_map = NULL;
static uint16_t bind_next_alloc = 0;

static void socket_bind_map_init(void) {
    if (bind_map) return;
    bind_map = hash_map_create(256);
    bind_next_alloc = 0;
    for (uint32_t i = 0; i < SOCKET_BIND_MAX; ++i) bind_entries[i].next = SOCKET_BIND_LIST_END;
}

static uint16_t socket_bind_head(protocol_t protocol, uint16_t port) {
    uint32_t key = ((uint32_t)protocol << 16) | port;
    void* value = hash_map_get(bind_map, &key, sizeof(key));
    if (!value) return SOCKET_BIND_LIST_END;
    return (uint16_t)((uintptr_t)value - 1);
}

static bool socket_bind_set_head(protocol_t protocol, uint16_t port, uint16_t head) {
    uint32_t key = ((uint32_t)protocol << 16) | port;
    if (head == SOCKET_BIND_LIST_END) {
        void* old = NULL;
        hash_map_remove(bind_map, &key, sizeof(key), &old);
        return true;
    }
    return hash_map_put(bind_map, &key, sizeof(key), (void*)(uintptr_t)(head + 1)) >= 0;
}

bool socket_bind_normalize_spec(SockBindSpec* spec) {
    if (!spec) return false;

    if (spec->kind == BIND_L3 && !spec->l3_id && !spec->ifindex && !spec->ver && ipv6_is_unspecified(spec->ip)) {
        spec->kind = BIND_ANY;
        return true;
    }

    if (spec->kind != BIND_IP) return true;

    if (spec->ver == IP_VER4) {
        uint32_t ip = 0;
        memcpy(&ip, spec->ip, 4);
        if (ipv4_is_unspecified(ip)) {
            spec->kind = BIND_ANY4;
            return true;
        }
        if (ipv4_is_multicast(ip) || ipv4_is_limited_broadcast(ip)) return false;
        return true;
    }

    if (spec->ver == IP_VER6) {
        if (ipv6_is_unspecified(spec->ip)) {
            spec->kind = BIND_ANY6;
            return true;
        }
        if (ipv6_is_multicast(spec->ip)) return false;
        return true;
    }

    if (!spec->ver && ipv6_is_unspecified(spec->ip)) {
        spec->kind = BIND_ANY;
        return true;
    }

    return false;
}

static bool socket_bind_l3_valid(ip_version_t ver, uint8_t l3_id) {
    if (!l3_id) return false;
    if (ver == IP_VER4) return ipv4_l3_is_active(l3_ipv4_find_by_id(l3_id));
    if (ver == IP_VER6) return ipv6_l3_is_ready(l3_ipv6_find_by_id(l3_id));
    return false;
}

uint32_t socket_bind_select_l3(const SockBindSpec* spec, ip_version_t ver, uint8_t* out, uint32_t cap) {
    if (!spec || !out || !cap || (ver != IP_VER4 && ver != IP_VER6)) return 0;

    uint32_t count = 0;
    SockBindSpec normal = *spec;
    if (!socket_bind_normalize_spec(&normal)) return 0;

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
            if (ipv4_is_multicast(ip) || ipv4_is_limited_broadcast(ip)) return 0;
            if (!ipv4_is_unspecified(ip)) {
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(ip);
                if (v4 && socket_bind_l3_valid(IP_VER4, v4->l3_id)) out[count++] = v4->l3_id;
                return count;
            }
        } else {
            if (ipv6_is_multicast(normal.ip)) return 0;
            if (!ipv6_is_unspecified(normal.ip)) {
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

bool socket_bind_insert(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, uint16_t port, socket_bind_token_t* out_token) {
    if (out_token) *out_token = 0;
    if (!socket || !spec || !port) return false;
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return false;

    SockBindSpec normal = *spec;
    if (!socket_bind_normalize_spec(&normal)) return false;

    socket_bind_map_init();
    if (!bind_map) return false;

    irq_flags_t irq = irq_save_disable();
    uint16_t head = socket_bind_head(protocol, port);
    for (uint16_t idx = head; idx != SOCKET_BIND_LIST_END; idx = bind_entries[idx].next) {
        socket_bind_entry_t* e = &bind_entries[idx];
        if (!e->used || e->protocol != protocol || e->port != port) continue;

        ip_version_t a_family = 0;
        ip_version_t b_family = 0;
        if (normal.kind == BIND_ANY4) a_family = IP_VER4;
        else if (normal.kind == BIND_ANY6) a_family = IP_VER6;
        else a_family = normal.ver;
        if (e->spec.kind == BIND_ANY4) b_family = IP_VER4;
        else if (e->spec.kind == BIND_ANY6) b_family = IP_VER6;
        else b_family = e->spec.ver;
        if (a_family && b_family && a_family != b_family) continue;

        bool overlap = true;
        if (normal.kind == BIND_IP && e->spec.kind == BIND_IP && a_family && b_family) {
            if (a_family == IP_VER4) {
                uint32_t av = 0;
                uint32_t bv = 0;
                memcpy(&av, normal.ip, 4);
                memcpy(&bv, e->spec.ip, 4);
                overlap = ipv4_is_unspecified(av) || ipv4_is_unspecified(bv) || av == bv;
            } else if (a_family == IP_VER6) overlap = ipv6_is_unspecified(normal.ip) || ipv6_is_unspecified(e->spec.ip) || memcmp(normal.ip, e->spec.ip, 16) == 0;
        } else if (normal.kind == BIND_L3 && e->spec.kind == BIND_L3) overlap = normal.l3_id == e->spec.l3_id;
        else if (normal.kind == BIND_L2 && e->spec.kind == BIND_L2) overlap = normal.ifindex == e->spec.ifindex;
        if (!overlap) continue;

        irq_restore(irq);
        return false;
    }

    uint16_t idx = SOCKET_BIND_LIST_END;
    for (uint32_t i = 0; i < SOCKET_BIND_MAX; ++i) {
        uint16_t cand = (uint16_t)((bind_next_alloc + i) % SOCKET_BIND_MAX);
        if (bind_entries[cand].used) continue;
        idx = cand;
        bind_next_alloc = (uint16_t)((cand + 1) % SOCKET_BIND_MAX);
        break;
    }
    if (idx == SOCKET_BIND_LIST_END) {
        irq_restore(irq);
        return false;
    }

    socket_bind_entry_t* e = &bind_entries[idx];
    uint32_t generation = (e->generation + 1) & 0xFFFF;
    if (!generation) generation = 1;
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->protocol = protocol;
    e->port = port;
    e->generation = generation;
    e->spec = normal;
    e->socket = socket;
    e->next = head;
    if (!socket_bind_set_head(protocol, port, idx)) {
        memset(e, 0, sizeof(*e));
        e->generation = generation;
        e->next = SOCKET_BIND_LIST_END;
        irq_restore(irq);
        return false;
    }

    socket_core_ref(socket);
    if (out_token) *out_token = (generation << 16) | (uint32_t)(idx + 1);

    irq_restore(irq);
    return true;
}

void socket_bind_remove(socket_bind_token_t token) {
    if (!token) return;

    uint32_t idxplus = (uint16_t)token;
    uint32_t generation = token >> 16;
    if (!idxplus || idxplus > SOCKET_BIND_MAX || !generation) return;

    uint16_t idx = (uint16_t)(idxplus - 1);
    ksocket_t* drop = NULL;

    socket_bind_map_init();
    if (!bind_map) return;

    irq_flags_t irq = irq_save_disable(); //TODO lock
    socket_bind_entry_t* e = &bind_entries[idx];
    if (e->used && e->generation == generation) {
        uint16_t head = socket_bind_head(e->protocol, e->port);
        uint16_t cur = head;
        uint16_t prev = SOCKET_BIND_LIST_END;
        while (cur != SOCKET_BIND_LIST_END) {
            if (cur == idx) {
                if (prev == SOCKET_BIND_LIST_END) head = bind_entries[cur].next;
                else bind_entries[prev].next = bind_entries[cur].next;
                socket_bind_set_head(e->protocol, e->port, head);
                break;
            }
            prev = cur;
            cur = bind_entries[cur].next;
        }

        drop = e->socket;
        memset(e, 0, sizeof(*e));
        e->generation = generation;
        e->next = SOCKET_BIND_LIST_END;
        bind_next_alloc = idx;
    }
    irq_restore(irq);

    if (drop) socket_core_put(drop);
}

int socket_bind_alloc_ephemeral(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, socket_bind_token_t* out_token) {
    if (out_token) *out_token = 0;
    if (!socket || !spec) return -1;
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return -1;

    rng_t rng;
    rng_init_random(&rng);
    uint32_t seed = rng_next32(&rng);

    uint32_t minp = SOCKET_PORT_MIN_EPHEMERAL;
    uint32_t maxp = SOCKET_PORT_MAX_EPHEMERAL;
    uint32_t range = maxp - minp + 1;
    uint32_t first = minp + (seed % range);

    for (uint32_t i = 0; i < range; ++i) {
        uint16_t port = (uint16_t)(minp + ((first - minp + i) % range));
        if (socket_bind_insert(socket, protocol, spec, port, out_token)) return port;
    }

    return -1;
}

int socket_bind_alloc_ephemeral_l3(ksocket_t* socket, protocol_t protocol, uint8_t l3_id, socket_bind_token_t* out_token) {
    if (!socket || !l3_id) return -1;

    SockBindSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = BIND_L3;
    spec.ver = l3_is_v6_from_id(l3_id) ? IP_VER6 : IP_VER4;
    spec.l3_id = l3_id;

    return socket_bind_alloc_ephemeral(socket, protocol, &spec, out_token);
}

uint32_t socket_bind_collect(protocol_t protocol, ip_version_t ipver, uint8_t l3_id, uint8_t ifindex, const void* dst_ip_addr, uint16_t dst_port, ksocket_t** out, uint32_t out_cap) {
    if (!out || !out_cap || !dst_ip_addr) return 0;
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return 0;

    uint32_t count = 0;
    socket_bind_map_init();
    if (!bind_map) return 0;

    irq_flags_t irq = irq_save_disable(); //TODO lock
    for (uint16_t idx = socket_bind_head(protocol, dst_port); idx != SOCKET_BIND_LIST_END && count < out_cap; idx = bind_entries[idx].next) {
        socket_bind_entry_t* e = &bind_entries[idx];
        if (!e->used || e->protocol != protocol || e->port != dst_port || !e->socket) continue;

        ip_version_t family = 0;
        if (e->spec.kind == BIND_ANY4) family = IP_VER4;
        else if (e->spec.kind == BIND_ANY6) family = IP_VER6;
        else family = e->spec.ver;
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

bool socket_bind_build_ipv4_tx_plan(const SockBindSpec* spec, bool use_spec, uint32_t dst, ipv4_tx_plan_t* out) {
    if (!out) return false;

    ip_tx_opts_t tx;
    memset(&tx, 0, sizeof(tx));
    const ip_tx_opts_t* hint = NULL;

    if (use_spec) {
        if (!spec) return 0;

        SockBindSpec normal = *spec;
        if (!socket_bind_normalize_spec(&normal)) return false;
        if (normal.kind == BIND_ANY6) return false;
        if (normal.ver && normal.ver != IP_VER4 && normal.kind != BIND_ANY && normal.kind != BIND_ANY4) return false;

        if (normal.kind == BIND_L3) {
            if (!socket_bind_l3_valid(IP_VER4, normal.l3_id)) return false;
            tx.scope = IP_TX_BOUND_L3;
            tx.index = normal.l3_id;
            hint = &tx;
        } else if (normal.kind == BIND_L2) {
            l2_interface_t* l2 = l2_interface_find_by_index(normal.ifindex);
            if (!l2 || !l2->is_up) return false;
            tx.scope = IP_TX_BOUND_L2;
            tx.index = normal.ifindex;
            hint = &tx;
        } else if (normal.kind == BIND_IP) {
            uint32_t ip = 0;
            memcpy(&ip, normal.ip, 4);
            if (ipv4_is_multicast(ip) || ipv4_is_limited_broadcast(ip)) return false;
            if (!ipv4_is_unspecified(ip)) {
                l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(ip);
                if (!v4 || !socket_bind_l3_valid(IP_VER4, v4->l3_id)) return false;
                tx.scope = IP_TX_BOUND_L3;
                tx.index = v4->l3_id;
                hint = &tx;
            }
        } else if (normal.kind != BIND_ANY && normal.kind != BIND_ANY4) return false;
    }

    if (!ipv4_build_tx_plan(dst, hint, out)) return false;
    return ipv4_tx_plan_valid(out);
}

bool socket_bind_build_ipv6_tx_plan(const SockBindSpec* spec, bool use_spec, const uint8_t dst[16], ipv6_tx_plan_t* out) {
    if (!out || !dst) return false;

    ip_tx_opts_t tx;
    memset(&tx, 0, sizeof(tx));
    const ip_tx_opts_t* hint = NULL;

    if (use_spec) {
        if (!spec) return false;

        SockBindSpec normal = *spec;
        if (!socket_bind_normalize_spec(&normal)) return false;
        if (normal.kind == BIND_ANY4) return false;
        if (normal.ver && normal.ver != IP_VER6 && normal.kind != BIND_ANY && normal.kind != BIND_ANY6) return false;

        if (normal.kind == BIND_L3) {
            if (!socket_bind_l3_valid(IP_VER6, normal.l3_id)) return false;
            tx.scope = IP_TX_BOUND_L3;
            tx.index = normal.l3_id;
            hint = &tx;
        } else if (normal.kind == BIND_L2) {
            l2_interface_t* l2 = l2_interface_find_by_index(normal.ifindex);
            if (!l2 || !l2->is_up) return false;
            tx.scope = IP_TX_BOUND_L2;
            tx.index = normal.ifindex;
            hint = &tx;
        } else if (normal.kind == BIND_IP) {
            if (ipv6_is_multicast(normal.ip)) return false;
            if (!ipv6_is_unspecified(normal.ip)) {
                l3_ipv6_interface_t* v6 = l3_ipv6_find_by_ip(normal.ip);
                if (!v6 || !socket_bind_l3_valid(IP_VER6, v6->l3_id)) return false;
                tx.scope = IP_TX_BOUND_L3;
                tx.index = v6->l3_id;
                hint = &tx;
            }
        } else if (normal.kind != BIND_ANY && normal.kind != BIND_ANY6) return false;
    }

    if (!ipv6_build_tx_plan(dst, hint, out)) return false;
    return ipv6_tx_plan_valid(out);
}