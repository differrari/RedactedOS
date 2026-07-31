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

typedef struct socket_bind_entry {
    bool used;
    bool listening;
    protocol_t protocol;
    uint16_t port;
    uint16_t next;
    uint16_t generation;
    SockBindSpec spec;
    uint32_t options;
    uint16_t owner_pid;
    net_l4_endpoint remote;
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

    if (spec->kind == BIND_L3) {
        if (!spec->l3_id) return false;
        if (!spec->ver) spec->ver = l3_is_v6_from_id(spec->l3_id) ? IP_VER6 : IP_VER4;
        return spec->ver == IP_VER4 || spec->ver == IP_VER6;
    }

    if (spec->kind == BIND_L2) return !spec->ver || spec->ver == IP_VER4 || spec->ver == IP_VER6;
    if (spec->kind == BIND_ANY) {
        spec->ver = 0;
        return true;
    }
    if (spec->kind == BIND_ANY4) {
        spec->ver = IP_VER4;
        return true;
    }
    if (spec->kind == BIND_ANY6) {
        spec->ver = IP_VER6;
        return true;
    }
    if (spec->kind != BIND_IP) return false;

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

bool socket_bind_insert(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, uint16_t port, uint32_t options, bool allow_reuse, socket_bind_token_t* out_token) {
    if (out_token) *out_token = 0;
    if (!socket || !spec || !port) return false;
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return false;

    SockBindSpec normal = *spec;
    if (!socket_bind_normalize_spec(&normal)) return false;

    if (normal.kind == BIND_L3) {
        if (!socket_bind_l3_valid(normal.ver, normal.l3_id)) return false;
        normal.ifindex = l3_ifindex_from_id(normal.l3_id);
    } else if (normal.kind == BIND_L2) {
        if (!l2_interface_find_by_index(normal.ifindex)) return false;
    } else if (normal.kind == BIND_IP) {
        if (normal.ver == IP_VER4) {
            uint32_t ip = 0;
            memcpy(&ip, normal.ip, 4);
            l3_ipv4_interface_t* v4 = l3_ipv4_find_by_ip(ip);
            if (!v4 || !v4->l2) return false;
            normal.l3_id = v4->l3_id;
            normal.ifindex = v4->l2->ifindex;
        } else if (normal.ver == IP_VER6) {
            l3_ipv6_interface_t* v6 = l3_ipv6_find_by_ip(normal.ip);
            if (!v6 || !v6->l2) return false;
            normal.l3_id = v6->l3_id;
            normal.ifindex = v6->l2->ifindex;
        } else return false;
    }

    socket_bind_map_init();
    if (!bind_map) return false;

    uint16_t owner_pid = protocol == PROTO_UDP ? socket_core_pid(socket) : 0;
    uint32_t reuse = options & SOCK_OPT_REUSEADDR;
    if (protocol == PROTO_UDP) reuse |= options & SOCK_OPT_REUSEPORT;
    irq_flags_t irq = irq_save_disable();

    if (protocol == PROTO_TCP && tcp_bind_conflicts(&normal, port, allow_reuse && reuse != 0)) {
        irq_restore(irq);
        return false;
    }

    uint16_t head = socket_bind_head(protocol, port);
    for (uint16_t idx = head; idx != SOCKET_BIND_LIST_END; idx = bind_entries[idx].next) {
        socket_bind_entry_t* other = &bind_entries[idx];
        if (!other->used || other->protocol != protocol || other->port != port) continue;
        if (!socket_bind_specs_overlap(&normal, &other->spec)) continue;

        bool allowed = allow_reuse && (reuse & SOCK_OPT_REUSEADDR) && (other->options & SOCK_OPT_REUSEADDR);
        if (protocol == PROTO_UDP && !allowed) {
            allowed = allow_reuse && owner_pid == other->owner_pid && (reuse & SOCK_OPT_REUSEPORT) && (other->options & SOCK_OPT_REUSEPORT);
        }
        if (protocol == PROTO_TCP && other->listening) allowed = false;
        if (!allowed) {
            irq_restore(irq);
            return false;
        }
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
    uint16_t generation = (uint16_t)(e->generation + 1);
    if (!generation) generation = 1;
    memset(e, 0, sizeof(*e));
    e->used = true;
    e->protocol = protocol;
    e->port = port;
    e->generation = generation;
    e->spec = normal;
    e->options = reuse;
    e->owner_pid = owner_pid;
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
    if (out_token) *out_token = ((uint32_t)generation << 16) | (uint32_t)(idx + 1);

    irq_restore(irq);
    return true;
}

bool socket_bind_tcp_listen(socket_bind_token_t token) {
    if (!token) return false;

    uint32_t idxplus = (uint16_t)token;
    uint32_t generation = token >> 16;
    if (!idxplus || idxplus > SOCKET_BIND_MAX || !generation) return false;

    socket_bind_map_init();
    if (!bind_map) return false;

    uint16_t idx = (uint16_t)(idxplus - 1);
    irq_flags_t irq = irq_save_disable();
    socket_bind_entry_t* entry = &bind_entries[idx];
    if (!entry->used || entry->generation != generation || entry->protocol != PROTO_TCP || !entry->socket) {
        irq_restore(irq);
        return false;
    }
    if (entry->listening) {
        irq_restore(irq);
        return true;
    }

    uint16_t head = socket_bind_head(PROTO_TCP, entry->port);
    for (uint16_t other_idx = head; other_idx != SOCKET_BIND_LIST_END; other_idx = bind_entries[other_idx].next) {
        socket_bind_entry_t* other = &bind_entries[other_idx];
        if (other_idx == idx || !other->used || !other->listening || other->protocol != PROTO_TCP || other->port != entry->port) continue;
        if (!socket_bind_specs_overlap(&entry->spec, &other->spec)) continue;
        irq_restore(irq);
        return false;
    }

    entry->listening = true;
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

void socket_bind_udp_set_remote(socket_bind_token_t token, const net_l4_endpoint* remote) {
    if (!token) return;

    uint32_t idxplus = (uint16_t)token;
    uint32_t generation = token >> 16;
    if (!idxplus || idxplus > SOCKET_BIND_MAX || !generation) return;

    uint16_t idx = (uint16_t)(idxplus-1);
    irq_flags_t irq = irq_save_disable();
    socket_bind_entry_t* e = &bind_entries[idx];
    if (e->used && e->generation == generation && e->protocol == PROTO_UDP) {
        memset(&e->remote, 0, sizeof(e->remote));
        if (remote && remote->port && (remote->ver == IP_VER4 || remote->ver == IP_VER6)) e->remote = *remote;
    }
    irq_restore(irq);
}

int socket_bind_alloc_ephemeral(ksocket_t* socket, protocol_t protocol, const SockBindSpec* spec, uint32_t options, socket_bind_token_t* out_token) {
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
        if (socket_bind_insert(socket, protocol, spec, port, options, false, out_token)) return port;
    }

    return -1;
}

int socket_bind_alloc_ephemeral_l3(ksocket_t* socket, protocol_t protocol, uint8_t l3_id, uint32_t options, socket_bind_token_t* out_token) {
    if (!socket || !l3_id) return -1;

    SockBindSpec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = BIND_L3;
    spec.ver = l3_is_v6_from_id(l3_id) ? IP_VER6 : IP_VER4;
    spec.l3_id = l3_id;

    return socket_bind_alloc_ephemeral(socket, protocol, &spec, options, out_token);
}

ksocket_t* socket_bind_lookup(protocol_t protocol, ip_version_t ipver, uint8_t l3_id, uint8_t ifindex, const void* src_ip_addr, uint16_t src_port, const void* dst_ip_addr, uint16_t dst_port) {
    if (!dst_ip_addr || (protocol != PROTO_TCP && protocol != PROTO_UDP)) return NULL;
    if (ipver != IP_VER4 && ipver != IP_VER6) return NULL;

    socket_bind_map_init();
    if (!bind_map) return NULL;
   //are all these fields needed?
    net_l4_endpoint src = {0};
    net_l4_endpoint dst = {0};
    src.ver = ipver;
    src.port = src_port;
    dst.ver = ipver;
    dst.port = dst_port;
    uint32_t ip_len = ipver == IP_VER6 ? 16 : 4;
    if (src_ip_addr) memcpy(src.ip, src_ip_addr, ip_len);
    memcpy(dst.ip, dst_ip_addr, ip_len);

    irq_flags_t irq = irq_save_disable(); //TODO lock
    uint16_t head = socket_bind_head(protocol, dst_port);
    int32_t best_score = 0;
    uint32_t best_count = 0;
    bool reuseport_group = false;
    uint16_t reuseport_owner = 0;
    uint64_t reuseport_hash = 0;
    socket_bind_entry_t* first = NULL;
    socket_bind_entry_t* reuseport_selected = NULL;

    for (uint16_t idx = head; idx != SOCKET_BIND_LIST_END; idx = bind_entries[idx].next) {
        socket_bind_entry_t* e = &bind_entries[idx];
        if (!e->used || e->protocol != protocol || e->port != dst_port || !e->socket || socket_core_is_closing(e->socket)) continue;
        if (protocol == PROTO_TCP && !e->listening) continue;
        if ((e->spec.kind == BIND_L3 || e->spec.kind == BIND_L2) && e->spec.ver && e->spec.ver != ipver) continue;

        int32_t score = 0;
        switch (e->spec.kind) {
            case BIND_IP:
                if (e->spec.ver == ipver && memcmp(e->spec.ip, dst_ip_addr, ip_len) == 0) score = 5;
                break;
            case BIND_L3:
                if (e->spec.l3_id == l3_id) score = 4;
                break;
            case BIND_L2:
                if (e->spec.ifindex == ifindex) score = 3;
                break;
            case BIND_ANY4:
                if (ipver == IP_VER4) score = 2;
                break;
            case BIND_ANY6:
                if (ipver == IP_VER6) score = 2;
                break;
            case BIND_ANY:
                score = 1;
                break;
            default:
                break;
        }
        if (!score) continue;

        if (protocol == PROTO_UDP && e->remote.port) {
            if (!src_ip_addr || e->remote.ver != ipver || e->remote.port != src_port) continue;
            if (memcmp(e->remote.ip, src_ip_addr, ip_len) != 0) continue;
            score += 8;
        }
        if (score < best_score) continue;

        bool reuseport = protocol == PROTO_UDP && (e->options & SOCK_OPT_REUSEPORT);
        if (score > best_score) {
            best_score = score;
            best_count = 1;
            reuseport_group = reuseport;
            reuseport_owner = e->owner_pid;
            reuseport_hash = 0;
            first = e;
            reuseport_selected = NULL;
        } else {
            best_count++;
            if (!reuseport || e->owner_pid != reuseport_owner) reuseport_group = false;
        }

        if (reuseport) {
            struct {
                net_l4_endpoint src;
                net_l4_endpoint dst;
                socket_handle_t handle;
            } key;
            memset(&key, 0, sizeof(key));
            key.src = src;
            key.dst = dst;
            key.handle = socket_core_export_handle(e->socket);
            uint64_t hash = hash_map_fnv1a64(&key, sizeof(key));
            if (!reuseport_selected || hash > reuseport_hash) {
                reuseport_selected = e;
                reuseport_hash = hash;
            }
        }
    }

    socket_bind_entry_t* selected = best_count > 1 && reuseport_group ? reuseport_selected : first;
    ksocket_t* socket = selected ? selected->socket : NULL;
    if (socket) socket_core_ref(socket);
    irq_restore(irq);
    return socket;
}

ksocket_t* socket_bind_udp_next_fanout(ip_version_t ipver, uint8_t l3_id, uint8_t ifindex, const void* dst_ip_addr, uint16_t dst_port, uint32_t* cursor) {
    if (!cursor || !dst_ip_addr || (ipver != IP_VER4 && ipver != IP_VER6)) return NULL;

    uint32_t start = *cursor;
    if (start >= SOCKET_BIND_MAX) return NULL;

    irq_flags_t irq = irq_save_disable();
    for (uint32_t i = start; i < SOCKET_BIND_MAX; ++i) {
        socket_bind_entry_t* e = &bind_entries[i];
        if (!e->used || e->protocol != PROTO_UDP || e->port != dst_port || !e->socket || socket_core_is_closing(e->socket)) continue;
        if ((e->spec.kind == BIND_L3 || e->spec.kind == BIND_L2) && e->spec.ver && e->spec.ver != ipver) continue;

        bool match = false;
        switch (e->spec.kind) {
            case BIND_IP:
                match = e->spec.ver == ipver && memcmp(e->spec.ip, dst_ip_addr, ipver == IP_VER6 ? 16 : 4) == 0;
                break;
            case BIND_L3:
                match = e->spec.l3_id == l3_id;
                break;
            case BIND_L2:
                match = e->spec.ifindex == ifindex;
                break;
            case BIND_ANY4:
                match = ipver == IP_VER4;
                break;
            case BIND_ANY6:
                match = ipver == IP_VER6;
                break;
            case BIND_ANY:
                match = true;
                break;
            default:
                break;
        }
        if (!match) continue;

        *cursor = i++;
        socket_core_ref(e->socket);
        irq_restore(irq);
        return e->socket;
    }

    *cursor = SOCKET_BIND_MAX;
    irq_restore(irq);
    return NULL;
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