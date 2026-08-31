#include "socket_bind.h"
#include "networking/transport_layer/tcp.h"
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
#define SOCKET_BIND_FLAG_REUSEADDR (1u << 0)
#define SOCKET_BIND_FLAG_REUSEPORT (1u << 1)
#define SOCKET_BIND_FLAG_LISTENING (1u << 2)

typedef struct socket_bind_entry {
    uint16_t port;
    uint16_t next;
    uint16_t generation;
    uint8_t flags;
    SockBindSpec spec;
    net_l4_endpoint remote;
    ksocket_t* socket;
} socket_bind_entry_t;

static socket_bind_entry_t bind_entries[SOCKET_BIND_MAX];
static hash_map_t* bind_map = NULL;
static uint16_t bind_next_alloc = 0;

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
        return hash_map_remove(bind_map, &key, sizeof(key), &old);
    }
    return hash_map_put(bind_map, &key, sizeof(key), (void*)(uintptr_t)(head + 1)) >= 0;
}

static bool socket_bind_normalize_spec(SockBindSpec* spec) {
    if (!spec) return false;

    if (spec->kind == BIND_L3 && !spec->l3_id && !spec->ifindex && !spec->ver && ipv6_is_unspecified(spec->ip)) {
        memset(spec, 0, sizeof(*spec));
        spec->kind = BIND_ANY;
        return true;
    }

    if (spec->kind == BIND_L3) {
        if (!spec->l3_id) return false;
        l3_ipv4_interface_t *v4 = l3_ipv4_find_by_id(spec->l3_id);
        l3_ipv6_interface_t *v6 = l3_ipv6_find_by_id(spec->l3_id);
        if (!v4 && !v6) return false;
        if (!spec->ver) spec->ver = v6 ? IP_VER6 : IP_VER4;
        if ((spec->ver == IP_VER4 && !v4) || (spec->ver == IP_VER6 && !v6)) return false;
        if (spec->ver != IP_VER4 && spec->ver != IP_VER6) return false;
        spec->ifindex = 0;
        memset(spec->ip, 0, sizeof(spec->ip));
        return true;
    }

    if (spec->kind == BIND_L2) {
        if (!spec->ifindex || (spec->ver && spec->ver != IP_VER4 && spec->ver != IP_VER6)) return false;
        spec->l3_id = 0;
        memset(spec->ip, 0, sizeof(spec->ip));
        return true;
    }
    if (spec->kind == BIND_ANY) {
        memset(spec, 0, sizeof(*spec));
        spec->kind = BIND_ANY;
        return true;
    }
    if (spec->kind == BIND_ANY4) {
        memset(spec, 0, sizeof(*spec));
        spec->kind = BIND_ANY4;
        spec->ver = IP_VER4;
        return true;
    }
    if (spec->kind == BIND_ANY6) {
        memset(spec, 0, sizeof(*spec));
        spec->kind = BIND_ANY6;
        spec->ver = IP_VER6;
        return true;
    }
    if (spec->kind != BIND_IP) return false;

    spec->l3_id = 0;
    spec->ifindex = 0;
    if (spec->ver == IP_VER4) {
        uint32_t ip = 0;
        memcpy(&ip, spec->ip, 4);
        if (ipv4_is_unspecified(ip)) {
            memset(spec, 0, sizeof(*spec));
            spec->kind = BIND_ANY4;
            spec->ver = IP_VER4;
            return true;
        }
        if (ipv4_is_multicast(ip) || ipv4_is_limited_broadcast(ip)) return false;
        memset(spec->ip + 4, 0, 12);
        return true;
    }

    if (spec->ver == IP_VER6) {
        if (ipv6_is_unspecified(spec->ip)) {
            memset(spec, 0, sizeof(*spec));
            spec->kind = BIND_ANY6;
            spec->ver = IP_VER6;
            return true;
        }
        if (ipv6_is_multicast(spec->ip)) return false;
        return true;
    }

    if (!spec->ver && ipv6_is_unspecified(spec->ip)) {
        memset(spec, 0, sizeof(*spec));
        spec->kind = BIND_ANY;
        return true;
    }

    return false;
}

bool socket_bind_prepare_spec(SockBindSpec* spec, protocol_t protocol) {
    if (!spec || (protocol != PROTO_TCP && protocol != PROTO_UDP)) return false;
    if (!socket_bind_normalize_spec(spec)) return false;

    if (spec->kind == BIND_ANY || spec->kind == BIND_ANY4 || spec->kind == BIND_ANY6) return true;
    if (spec->kind == BIND_L2) {
        l2_interface_t* l2 = l2_interface_find_by_index(spec->ifindex);
        return l2 && l2->is_up;
    }

    if (spec->kind == BIND_L3) {
        if (spec->ver == IP_VER4) {
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(spec->l3_id);
            if (!v4 || !v4->l2 || (protocol == PROTO_TCP ? !ipv4_l3_is_ready(v4) : !ipv4_l3_is_active(v4))) return false;
            spec->ifindex = v4->l2->ifindex;
            return true;
        }
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(spec->l3_id);
        if (!v6 || !v6->l2 || (protocol == PROTO_TCP ? !ipv6_l3_is_tcp_usable(v6) : !ipv6_l3_is_ready(v6))) return false;
        spec->ifindex = v6->l2->ifindex;
        return true;
    }

    if (spec->ver == IP_VER4) {
        uint32_t ip = 0;
        memcpy(&ip, spec->ip, 4);
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(ip);
        if (!v4 || !v4->l2 || (protocol == PROTO_TCP ? !ipv4_l3_is_ready(v4) : !ipv4_l3_is_active(v4))) return false;
        spec->l3_id = v4->l3_id;
        spec->ifindex = v4->l2->ifindex;
        return true;
    }

    l3_ipv6_interface_t* v6 = l3_ipv6_find_by_ip(spec->ip);
    if (!v6 || !v6->l2 || (protocol == PROTO_TCP ? !ipv6_l3_is_tcp_usable(v6) : !ipv6_l3_is_ready(v6))) return false;
    spec->l3_id = v6->l3_id;
    spec->ifindex = v6->l2->ifindex;
    return true;
}

uint8_t socket_bind_match_score(const SockBindSpec* spec, ip_version_t ver, l3_id_t l3_id, uint8_t ifindex, const void* ip_addr) {
    if (!spec || !ip_addr || (ver != IP_VER4 && ver != IP_VER6)) return 0;
    if ((spec->kind == BIND_L3 || spec->kind == BIND_L2) && spec->ver && spec->ver != ver) return 0;

    switch (spec->kind) {
        case BIND_IP:
            if (spec->ver != ver) return 0;
            return memcmp(spec->ip, ip_addr, ver == IP_VER6 ? 16 : 4) == 0 ? 5 : 0;
        case BIND_L3:
            return spec->l3_id == l3_id ? 4 : 0;
        case BIND_L2:
            return spec->ifindex == ifindex ? 3 : 0;
        case BIND_ANY4:
            return ver == IP_VER4 ? 2 : 0;
        case BIND_ANY6:
            return ver == IP_VER6 ? 2 : 0;
        case BIND_ANY:
            return 1;
        default:
            return 0;
    }
}

static bool socket_bind_specs_overlap(const SockBindSpec* a, const SockBindSpec* b) {
    if (!a || !b) return false;

    ip_version_t af = a->kind == BIND_ANY4 ? IP_VER4 : a->kind == BIND_ANY6 ? IP_VER6 : a->ver;
    ip_version_t bf = b->kind == BIND_ANY4 ? IP_VER4 : b->kind == BIND_ANY6 ? IP_VER6 : b->ver;
    if (af && bf && af != bf) return false;
    if (a->kind == BIND_ANY || b->kind == BIND_ANY) return true;
    if (a->kind == BIND_ANY4 || a->kind == BIND_ANY6 || b->kind == BIND_ANY4 || b->kind == BIND_ANY6) return true;

    if (a->kind == b->kind) {
        if (a->kind == BIND_L3) return a->l3_id == b->l3_id;
        if (a->kind == BIND_L2) return a->ifindex == b->ifindex;
        if (a->kind == BIND_IP) return af == IP_VER4 ? memcmp(a->ip, b->ip, 4) == 0 : memcmp(a->ip, b->ip, 16) == 0;
        return true;
    }

    if (a->kind != BIND_L2 && b->kind != BIND_L2 && a->l3_id && b->l3_id) return a->l3_id == b->l3_id;
    return a->ifindex && b->ifindex && a->ifindex == b->ifindex;
}

static bool socket_bind_insert_prepared(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, uint16_t port, uint32_t options, bool allow_reuse, socket_bind_token_t* out_token) {
    if (!bind_map) {
        hash_map_t* new_map = hash_map_create(SOCKET_BIND_MAX * 2u);
        if (!new_map) return false;

        irq_flags_t init_irq = irq_save_disable();
        if (!bind_map) {
            bind_next_alloc = 0;
            for (uint32_t i = 0; i < SOCKET_BIND_MAX; ++i) bind_entries[i].next = SOCKET_BIND_LIST_END;
            bind_map = new_map;
            new_map = NULL;
        }
        irq_restore(init_irq);
        if (new_map) hash_map_destroy(new_map);
    }

    uint8_t flags = 0;
    if (options & SOCK_OPT_REUSEADDR) flags |= SOCKET_BIND_FLAG_REUSEADDR;
    if (protocol == PROTO_UDP && (options & SOCK_OPT_REUSEPORT)) flags |= SOCKET_BIND_FLAG_REUSEPORT;
    irq_flags_t irq = irq_save_disable();

    if (protocol == PROTO_TCP && tcp_bind_conflicts(spec, port, allow_reuse && (flags & SOCKET_BIND_FLAG_REUSEADDR))) {
        irq_restore(irq);
        return false;
    }

    uint16_t head = socket_bind_head(protocol, port);
    for (uint16_t idx = head; idx != SOCKET_BIND_LIST_END; idx = bind_entries[idx].next) {
        socket_bind_entry_t* other = &bind_entries[idx];
        if (!other->socket || other->port != port) continue;
        if (!socket_bind_specs_overlap(spec, &other->spec)) continue;

        bool allowed = allow_reuse && (flags & SOCKET_BIND_FLAG_REUSEADDR) && (other->flags & SOCKET_BIND_FLAG_REUSEADDR);
        if (protocol == PROTO_UDP && !allowed) {
            allowed = allow_reuse && socket_core_pid(socket) == socket_core_pid(other->socket) && (flags & SOCKET_BIND_FLAG_REUSEPORT) && (other->flags & SOCKET_BIND_FLAG_REUSEPORT);
        }
        if (protocol == PROTO_TCP && (other->flags & SOCKET_BIND_FLAG_LISTENING)) allowed = false;
        if (!allowed) {
            irq_restore(irq);
            return false;
        }
    }

    uint16_t idx = SOCKET_BIND_LIST_END;
    for (uint32_t i = 0; i < SOCKET_BIND_MAX; ++i) {
        uint16_t cand = (uint16_t)((bind_next_alloc + i) % SOCKET_BIND_MAX);
        if (bind_entries[cand].socket) continue;
        idx = cand;
        bind_next_alloc = (uint16_t)((cand + 1) % SOCKET_BIND_MAX);
        break;
    }
    if (idx == SOCKET_BIND_LIST_END) {
        irq_restore(irq);
        return false;
    }

    socket_bind_entry_t* entry = &bind_entries[idx];
    uint16_t generation = (uint16_t)(entry->generation + 1);
    if (!generation) generation = 1;
    memset(entry, 0, sizeof(*entry));
    entry->port = port;
    entry->generation = generation;
    entry->flags = flags;
    entry->spec = *spec;
    entry->socket = socket;
    entry->next = head;
    if (!socket_bind_set_head(protocol, port, idx)) {
        memset(entry, 0, sizeof(*entry));
        entry->generation = generation;
        entry->next = SOCKET_BIND_LIST_END;
        irq_restore(irq);
        return false;
    }

    socket_core_ref(socket);
    if (out_token) *out_token = ((uint32_t)generation << 16) | (uint32_t)(idx + 1);

    irq_restore(irq);
    return true;
}

bool socket_bind_insert(ksocket_t* socket, protocol_t protocol, SockBindSpec* spec, uint16_t port, uint32_t options, bool allow_reuse, socket_bind_token_t* out_token) {
    if (out_token) *out_token = 0;
    if (!socket || !spec || !port) return false;
    if (!socket_bind_prepare_spec(spec, protocol)) return false;
    return socket_bind_insert_prepared(socket, protocol, spec, port, options, allow_reuse, out_token);
}

bool socket_bind_tcp_listen(socket_bind_token_t token) {
    if (!token) return false;

    uint32_t idxplus = (uint16_t)token;
    uint32_t generation = token >> 16;
    if (!idxplus || idxplus > SOCKET_BIND_MAX || !generation) return false;

    if (!bind_map) return false;

    uint16_t idx = (uint16_t)(idxplus - 1);
    irq_flags_t irq = irq_save_disable();
    socket_bind_entry_t* entry = &bind_entries[idx];
    if (!entry->socket || entry->generation != generation || socket_core_protocol(entry->socket) != PROTO_TCP) {
        irq_restore(irq);
        return false;
    }
    if (entry->flags & SOCKET_BIND_FLAG_LISTENING) {
        irq_restore(irq);
        return true;
    }

    uint16_t head = socket_bind_head(PROTO_TCP, entry->port);
    for (uint16_t other_idx = head; other_idx != SOCKET_BIND_LIST_END; other_idx = bind_entries[other_idx].next) {
        socket_bind_entry_t* other = &bind_entries[other_idx];
        if (other_idx == idx || !other->socket || !(other->flags & SOCKET_BIND_FLAG_LISTENING) || other->port != entry->port) continue;
        if (!socket_bind_specs_overlap(&entry->spec, &other->spec)) continue;
        irq_restore(irq);
        return false;
    }

    entry->flags |= SOCKET_BIND_FLAG_LISTENING;
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

    if (!bind_map) return;

    irq_flags_t irq = irq_save_disable(); //TODO lock
    socket_bind_entry_t* e = &bind_entries[idx];
    if (e->socket && e->generation == generation) {
        protocol_t protocol = socket_core_protocol(e->socket);
        uint16_t head = socket_bind_head(protocol, e->port);
        uint16_t cur = head;
        uint16_t prev = SOCKET_BIND_LIST_END;
        bool removed = false;
        while (cur != SOCKET_BIND_LIST_END) {
            if (cur == idx) {
                if (prev == SOCKET_BIND_LIST_END) {
                    head = bind_entries[cur].next;
                    removed = socket_bind_set_head(protocol, e->port, head);
                } else {
                    bind_entries[prev].next = bind_entries[cur].next;
                    removed = true;
                }
                break;
            }
            prev = cur;
            cur = bind_entries[cur].next;
        }

        if (removed) {
            drop = e->socket;
            memset(e, 0, sizeof(*e));
            e->generation = generation;
            e->next = SOCKET_BIND_LIST_END;
            bind_next_alloc = idx;
        }
    }
    irq_restore(irq);

    if (drop) socket_core_put(drop);
}

void socket_bind_udp_set_remote(socket_bind_token_t token, const net_l4_endpoint* remote) {
    if (!token) return;

    uint32_t idxplus = (uint16_t)token;
    uint32_t generation = token >> 16;
    if (!idxplus || idxplus > SOCKET_BIND_MAX || !generation) return;

    uint16_t idx = (uint16_t)(idxplus-1);
    irq_flags_t irq = irq_save_disable();
    socket_bind_entry_t* e = &bind_entries[idx];
    if (e->socket && e->generation == generation && socket_core_protocol(e->socket) == PROTO_UDP) {
        memset(&e->remote, 0, sizeof(e->remote));
        if (remote && remote->port && (remote->ver == IP_VER4 || remote->ver == IP_VER6)) e->remote = *remote;
    }
    irq_restore(irq);
}

int32_t socket_bind_alloc_ephemeral(ksocket_t* socket, protocol_t protocol, SockBindSpec* spec, uint32_t options, socket_bind_token_t* out_token) {
    if (out_token) *out_token = 0;
    if (!socket || !spec || !socket_bind_prepare_spec(spec, protocol)) return -1;

    rng_t rng;
    rng_init_random(&rng);
    uint32_t seed = rng_next32(&rng);

    uint32_t minp = SOCKET_PORT_MIN_EPHEMERAL;
    uint32_t maxp = SOCKET_PORT_MAX_EPHEMERAL;
    uint32_t range = maxp - minp + 1;
    uint32_t first = minp + (seed % range);

    for (uint32_t i = 0; i < range; ++i) {
        uint16_t port = (uint16_t)(minp + ((first - minp + i) % range));
        if (socket_bind_insert_prepared(socket, protocol, spec, port, options, false, out_token)) return port;
    }

    return -1;
}

int32_t socket_bind_alloc_ephemeral_l3(ksocket_t* socket, protocol_t protocol, l3_id_t l3_id, uint32_t options, SockBindSpec* out_spec, socket_bind_token_t* out_token) {
    if (!socket || !l3_id || !out_spec) return -1;

    SockBindSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = BIND_L3;
    if (l3_ipv4_find_by_id(l3_id)) spec.ver = IP_VER4;
    else if (l3_ipv6_find_by_id(l3_id)) spec.ver = IP_VER6;
    else return -1;
    spec.l3_id = l3_id;

    int32_t port = socket_bind_alloc_ephemeral(socket, protocol, &spec, options, out_token);
    if (port >= 0) *out_spec = spec;
    return port;
}

ksocket_t* socket_bind_lookup(protocol_t protocol, ip_version_t ipver, l3_id_t l3_id, uint8_t ifindex, const void* src_ip_addr, uint16_t src_port, const void* dst_ip_addr, uint16_t dst_port) {
    if (!dst_ip_addr || (protocol != PROTO_TCP && protocol != PROTO_UDP)) return NULL;
    if (ipver != IP_VER4 && ipver != IP_VER6) return NULL;
    if (!bind_map) return NULL;

    uint32_t ip_len = ipver == IP_VER6 ? 16u : 4u;
    uint8_t reuseport_key[1 + sizeof(src_port) + sizeof(dst_port) + 32 + sizeof(socket_handle_t)];
    uint32_t reuseport_key_len = 0;

    irq_flags_t irq = irq_save_disable(); //TODO lock
    uint16_t head = socket_bind_head(protocol, dst_port);
    uint8_t best_score = 0;
    bool reuseport_group = false;
    uint16_t reuseport_owner = 0;
    uint64_t reuseport_hash = 0;
    socket_bind_entry_t* first = NULL;
    socket_bind_entry_t* reuseport_selected = NULL;

    for (uint16_t idx = head; idx != SOCKET_BIND_LIST_END; idx = bind_entries[idx].next) {
        socket_bind_entry_t* e = &bind_entries[idx];
        if (!e->socket || e->port != dst_port || socket_core_is_closing(e->socket)) continue;
        if (protocol == PROTO_TCP && !(e->flags & SOCKET_BIND_FLAG_LISTENING)) continue;

        uint8_t score = socket_bind_match_score(&e->spec, ipver, l3_id, ifindex, dst_ip_addr);
        if (!score) continue;

        if (protocol == PROTO_UDP && e->remote.port) {
            if (!src_ip_addr || e->remote.ver != ipver || e->remote.port != src_port) continue;
            if (memcmp(e->remote.ip, src_ip_addr, ip_len) != 0) continue;
            score += 8;
        }
        if (score < best_score) continue;

        bool reuseport = protocol == PROTO_UDP && (e->flags & SOCKET_BIND_FLAG_REUSEPORT);
        if (score > best_score) {
            best_score = score;
            reuseport_group = reuseport;
            reuseport_owner = socket_core_pid(e->socket);
            reuseport_hash = 0;
            first = e;
            reuseport_selected = NULL;
        } else if (!reuseport || socket_core_pid(e->socket) != reuseport_owner) reuseport_group = false;
        if (reuseport_group && reuseport) {
            if (!reuseport_key_len) {
                reuseport_key[reuseport_key_len++] = (uint8_t)ipver;
                memcpy(reuseport_key + reuseport_key_len, &src_port, sizeof(src_port));
                reuseport_key_len += sizeof(src_port);
                memcpy(reuseport_key + reuseport_key_len, &dst_port, sizeof(dst_port));
                reuseport_key_len += sizeof(dst_port);
                if (src_ip_addr) memcpy(reuseport_key + reuseport_key_len, src_ip_addr, ip_len);
                else memset(reuseport_key + reuseport_key_len, 0, ip_len);
                reuseport_key_len += ip_len;
                memcpy(reuseport_key + reuseport_key_len, dst_ip_addr, ip_len);
                reuseport_key_len += ip_len;
            }

            socket_handle_t handle = socket_core_export_handle(e->socket);
            memcpy(reuseport_key + reuseport_key_len, &handle, sizeof(handle));
            uint64_t hash = hash_map_fnv1a64(reuseport_key, reuseport_key_len + sizeof(handle));
            if (!reuseport_selected || hash > reuseport_hash) {
                reuseport_selected = e;
                reuseport_hash = hash;
            }
        }
    }

    socket_bind_entry_t* selected = reuseport_group && reuseport_selected ? reuseport_selected : first;
    ksocket_t* socket = selected ? selected->socket : NULL;
    if (socket) socket_core_ref(socket);
    irq_restore(irq);
    return socket;
}

ksocket_t* socket_bind_udp_next_fanout(ip_version_t ipver, l3_id_t l3_id, uint8_t ifindex, const void* dst_ip_addr, uint16_t dst_port, uint32_t* cursor) {
    if (!cursor || !dst_ip_addr || (ipver != IP_VER4 && ipver != IP_VER6)) return NULL;

    uint32_t start = *cursor;
    if (start >= SOCKET_BIND_MAX) return NULL;

    irq_flags_t irq = irq_save_disable();
    for (uint32_t i = start; i < SOCKET_BIND_MAX; ++i) {
        socket_bind_entry_t* e = &bind_entries[i];
        if (!e->socket || socket_core_protocol(e->socket) != PROTO_UDP || e->port != dst_port || socket_core_is_closing(e->socket)) continue;
        if (!socket_bind_match_score(&e->spec, ipver, l3_id, ifindex, dst_ip_addr)) continue;

        *cursor = i + 1;
        socket_core_ref(e->socket);
        irq_restore(irq);
        return e->socket;
    }

    *cursor = SOCKET_BIND_MAX;
    irq_restore(irq);
    return NULL;
}

static bool socket_bind_build_tx_opts(const SockBindSpec* spec, ip_version_t ver, ip_tx_opts_t* tx, const ip_tx_opts_t** hint) {
    if (!spec || !tx || !hint || (ver != IP_VER4 && ver != IP_VER6)) return false;
    
    SockBindSpec normal = *spec;
    if (!socket_bind_normalize_spec(&normal)) return false;
    if ((normal.kind == BIND_ANY4 && ver != IP_VER4) || (normal.kind == BIND_ANY6 && ver != IP_VER6)) return false;
    if (normal.ver && normal.ver != ver && normal.kind != BIND_ANY && normal.kind != BIND_ANY4 && normal.kind != BIND_ANY6) return false;

    memset(tx, 0, sizeof(*tx));
    *hint = NULL;
    if (normal.kind == BIND_ANY || normal.kind == BIND_ANY4 || normal.kind == BIND_ANY6) return true;
    if (normal.kind == BIND_L2) {
        l2_interface_t* l2 = l2_interface_find_by_index(normal.ifindex);
        if (!l2 || !l2->is_up) return false;
        tx->scope = IP_TX_BOUND_L2;
        tx->target.ifindex = normal.ifindex;
        *hint = tx;
        return true;
    }

    l3_id_t l3_id = normal.l3_id;
    if (normal.kind == BIND_IP) {
        if (ver == IP_VER4) {
            uint32_t ip = 0;
            memcpy(&ip, normal.ip, 4);
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(ip);
            if (!v4 || !v4->l2) return false;
            l3_id = v4->l3_id;
        } else {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_ip(normal.ip);
            if (!v6 || !v6->l2) return false;
            l3_id = v6->l3_id;
        }
    } else if (normal.kind != BIND_L3) return false;

    if (ver == IP_VER4) {
        l3_ipv4_interface_t* v4 = l3_ipv4_find_by_id(l3_id);
        if (!v4 || !v4->l2) return false;
    } else {
        l3_ipv6_interface_t* v6 = l3_ipv6_find_by_id(l3_id);
        if (!v6 || !v6->l2) return false;
    }
    tx->scope = IP_TX_BOUND_L3;
    tx->target.l3_id = l3_id;
    *hint = tx;
    return true;
}

bool socket_bind_build_ipv4_tx_plan(const SockBindSpec* spec, bool use_spec, uint32_t dst, ipv4_tx_plan_t* out) {
    if (!out) return false;

    ip_tx_opts_t tx;
    const ip_tx_opts_t* hint = NULL;
    if (use_spec && !socket_bind_build_tx_opts(spec, IP_VER4, &tx, &hint)) return false; 

    if (!ipv4_build_tx_plan(dst, hint, out)) return false;
    return ipv4_tx_plan_valid(out);
}

bool socket_bind_build_ipv6_tx_plan(const SockBindSpec* spec, bool use_spec, const uint8_t dst[16], ipv6_tx_plan_t* out) {
    if (!out || !dst) return false;

    ip_tx_opts_t tx;
    const ip_tx_opts_t* hint = NULL;
    if (use_spec && !socket_bind_build_tx_opts(spec, IP_VER6, &tx, &hint)) return false; 

    if (!ipv6_build_tx_plan(dst, hint, out)) return false;
    return ipv6_tx_plan_valid(out);
}