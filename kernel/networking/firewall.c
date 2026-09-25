#include "firewall.h"
#include "exceptions/irq.h"
#include "networking/internet_layer/ipv4_utils.h"
#include "networking/internet_layer/ipv6_utils.h"
#include "data/struct/queue.h"
#include "std/memory.h"
#include "net/socket_types.h"

typedef struct firewall_rule_entry {
    NetCtrlFirewallRule rule;
    uint16_t owner_pid;
} firewall_rule_entry_t;
static CQueue rules_in;
static CQueue rules_out;
static uint32_t next_id = 1;
static volatile bool enabled = true;
static uint8_t default_in = NET_CTRL_FIREWALL_DENY;
static uint8_t default_out = NET_CTRL_FIREWALL_ALLOW; 

bool firewall_allows(protocol_t protocol, NetCtrlFirewallDirection direction, const net_l4_endpoint* remote, uint16_t local_port, bool related) {
    if (protocol != PROTO_TCP && protocol != PROTO_UDP) return true;
    if (!remote || (direction != NET_CTRL_FIREWALL_IN && direction != NET_CTRL_FIREWALL_OUT)) return false;

    uint32_t remote_v4 = 0;
    if (remote->ver == IP_VER4) {
        memcpy(&remote_v4, remote->ip, sizeof(remote_v4));
        if (ipv4_is_loopback(remote_v4)) return true;
    } else if (remote->ver == IP_VER6) {
        if (ipv6_is_loopback(remote->ip)) return true;
    } else return false;

    if (related || !enabled) return true;

    irq_flags_t irq = irq_save_disable();
    if (!enabled) {
        irq_restore(irq);
        return true;
    }

    CQueue* rules = direction == NET_CTRL_FIREWALL_IN ? &rules_in : &rules_out;
    uint16_t port = direction == NET_CTRL_FIREWALL_IN ? local_port : remote->port;
    uint8_t action = 0;
    uint64_t index = rules->tail;
    for (uint64_t i = 0; i < rules->length; i++) {
        const firewall_rule_entry_t* entry = (const firewall_rule_entry_t*)((const uint8_t*)rules->buffer + index * rules->elem_size);
        const NetCtrlFirewallRule* rule = &entry->rule;
        if (++index == rules->capacity) index = 0;
        if (rule->protocol != PROTO_NONE && rule->protocol != protocol) continue;

        if (rule->ip_version) {
            if (remote->ver != rule->ip_version) continue;
            if (rule->prefix_len) {
                if (rule->ip_version == IP_VER4) {
                    uint32_t rule_v4 = 0;
                    memcpy(&rule_v4, rule->address, sizeof(rule_v4));
                    uint32_t mask = rule->prefix_len == 32 ? UINT32_MAX : UINT32_MAX << (32 - rule->prefix_len);
                    if ((rule_v4 & mask) != (remote_v4 & mask)) continue;
                } else {
                    uint8_t bytes = rule->prefix_len >> 3;
                    uint8_t bits = rule->prefix_len & 7;
                    if (bytes && memcmp(rule->address, remote->ip, bytes) != 0) continue;
                    if (bits) {
                        uint8_t mask = (uint8_t)(0xFFu << (8 - bits));
                        if ((rule->address[bytes] & mask) != (remote->ip[bytes] & mask)) continue;
                    }
                }
            }
        }
        
        if (rule->port_from && (port < rule->port_from || port > rule->port_to)) continue;

        action = rule->action;
        break;
    }

    uint8_t fallback = direction == NET_CTRL_FIREWALL_IN ? default_in : default_out;
    irq_restore(irq);
    return action ? action == NET_CTRL_FIREWALL_ALLOW : fallback == NET_CTRL_FIREWALL_ALLOW;
}

uint32_t firewall_get_snapshot(NetCtrlFirewallState* state, NetCtrlFirewallRule* out, uint32_t max_rules) {
    irq_flags_t irq = irq_save_disable();
    uint64_t total64 = rules_in.length + rules_out.length;
    uint32_t total = total64 > UINT32_MAX ? UINT32_MAX : (uint32_t)total64;
    if (state) *state = (NetCtrlFirewallState){ .enabled = enabled, .default_in = default_in, .default_out = default_out, .rule_count = total };
    if (!out || !max_rules) {
        irq_restore(irq);
        return total;
    }

    uint32_t count = 0;
    uint64_t in_i = 0, out_i = 0;
    while (count < max_rules && (in_i < rules_in.length || out_i < rules_out.length)) {
        const firewall_rule_entry_t* in_entry = NULL;
        const firewall_rule_entry_t* out_entry = NULL;
        if (in_i < rules_in.length) {
            uint64_t index = (rules_in.tail + in_i) % rules_in.capacity;
            in_entry = (const firewall_rule_entry_t*)((const uint8_t*)rules_in.buffer + index * rules_in.elem_size);
        }
        if (out_i < rules_out.length) {
            uint64_t index = (rules_out.tail + out_i) % rules_out.capacity;
            out_entry = (const firewall_rule_entry_t*)((const uint8_t*)rules_out.buffer + index * rules_out.elem_size);
        }
        bool take_in = in_entry && (!out_entry || in_entry->rule.id < out_entry->rule.id);
        out[count++] = (take_in ? in_entry : out_entry)->rule;
        if (take_in) in_i++;
        else out_i++;
    }
    if (state) state->rule_count = count;
    irq_restore(irq);
    return count;
}

int32_t firewall_add_rule(const NetCtrlFirewallRule* input, uint16_t owner_pid) {
    if (!input || (input->action != NET_CTRL_FIREWALL_ALLOW && input->action != NET_CTRL_FIREWALL_DENY) ||
        (input->direction != NET_CTRL_FIREWALL_IN && input->direction != NET_CTRL_FIREWALL_OUT) ||
        (input->protocol != PROTO_NONE && input->protocol != PROTO_TCP && input->protocol != PROTO_UDP) ||
        (input->ip_version && input->ip_version != IP_VER4 && input->ip_version != IP_VER6) ||
        (input->ip_version == IP_VER4 && input->prefix_len > 32) || (input->ip_version == IP_VER6 && input->prefix_len > 128) ||
        (!input->ip_version && input->prefix_len) || ((!input->port_from) != (!input->port_to)) ||
        (input->port_from && input->port_from > input->port_to)) return SOCK_ERR_INVAL;
        //the longest if statement in the world

    firewall_rule_entry_t entry = { .rule = *input, .owner_pid = owner_pid };
    irq_flags_t irq = irq_save_disable();
    CQueue* rules = input->direction == NET_CTRL_FIREWALL_IN ? &rules_in : &rules_out;
    if (!rules->elem_size)cqueue_init(rules, 0, sizeof(firewall_rule_entry_t), NULL, NULL);
    entry.rule.id = next_id++;
    if (!entry.rule.id) entry.rule.id = next_id++;
    if (!cqueue_enqueue(rules, &entry)) {
        irq_restore(irq);
        return SOCK_ERR_SYS;
    }
    irq_restore(irq);
    return SOCK_OK;
}

static void firewall_remove_at(CQueue* rules, uint64_t offset) {
    for (uint64_t i = offset; i + 1 < rules->length; i++) {
        uint64_t dst = (rules->tail + i) % rules->capacity;
        uint64_t src = (rules->tail + i + 1) % rules->capacity;
        memcpy((uint8_t*)rules->buffer + dst * rules->elem_size, (uint8_t*)rules->buffer + src * rules->elem_size, rules->elem_size);
    }
    rules->head = (rules->head + rules->capacity - 1) % rules->capacity;
    memset((uint8_t*)rules->buffer + rules->head * rules->elem_size, 0, rules->elem_size);
    rules->length = rules->length - 1;
}

int32_t firewall_remove_rule(uint32_t id) {
    if (!id) return SOCK_ERR_INVAL;
    irq_flags_t irq = irq_save_disable();
    CQueue* lists[2] = {&rules_in, &rules_out};
    for (uint32_t l = 0; l < 2; l++) {
        CQueue* rules = lists[l];
        for (uint64_t i = 0; i < rules->length; i++) {
            uint64_t index = (rules->tail + i) % rules->capacity;
            firewall_rule_entry_t* entry = (firewall_rule_entry_t*)((uint8_t*)rules->buffer + index * rules->elem_size);
            if (entry->rule.id != id) continue;
            firewall_remove_at(rules, i);
            irq_restore(irq);
            return SOCK_OK;
        }
    }
    irq_restore(irq);
    return SOCK_ERR_NOT_FOUND;
}

void firewall_cleanup_process(uint16_t pid) {
    if (!pid) return;
    irq_flags_t irq = irq_save_disable();
    CQueue* lists[2] = {&rules_in, &rules_out};
    for (uint32_t l = 0; l < 2; l++) {
        CQueue* rules = lists[l];
        for (uint64_t i = 0; i < rules->length;) {
            uint64_t index = (rules->tail + i) % rules->capacity;
            firewall_rule_entry_t* entry = (firewall_rule_entry_t*)((uint8_t*)rules->buffer + index * rules->elem_size);
            if (entry->owner_pid != pid) {
                i++;
                continue;
            }
            firewall_remove_at(rules, i);
        }
    }
    irq_restore(irq);
}

void firewall_clear_rules(void) {
    irq_flags_t irq = irq_save_disable();
    cqueue_clear(&rules_in);
    cqueue_clear(&rules_out);
    irq_restore(irq);
}

void firewall_set_enabled(bool value) {
    irq_flags_t irq = irq_save_disable();
    enabled = value;
    irq_restore(irq);
}

int32_t firewall_set_default(NetCtrlFirewallDirection direction, NetCtrlFirewallAction action) {
    if ((direction != NET_CTRL_FIREWALL_IN && direction != NET_CTRL_FIREWALL_OUT) || (action != NET_CTRL_FIREWALL_ALLOW && action != NET_CTRL_FIREWALL_DENY)) return SOCK_ERR_INVAL;
    irq_flags_t irq = irq_save_disable();
    if (direction == NET_CTRL_FIREWALL_IN) default_in = action;
    else default_out = action;
    irq_restore(irq);
    return SOCK_OK;
}