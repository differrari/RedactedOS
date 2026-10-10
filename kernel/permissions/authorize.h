#pragma once

#include "types.h"

typedef u32 auth_token;

typedef struct {
    u32 auth_token;
    u32 actions;
} auth_entry;

typedef union {
    struct {
        u64 type: 8;
        u64 id: 56;
    };
    u64 resource;
} auth_resource;

typedef struct process_t process_t;

typedef enum {
    auth_process_send_signals = 1 << 0,
    auth_process_intercept_exceptions = 1 << 1,
    auth_process_input = 1 << 2, // /proc/<id>/in
    auth_process_output = 1 << 3, // /proc/<id>/out
    
    auth_process_state = 1 << 4, // /proc/<id>/state
    auth_process_info = 1 << 5, // /proc/<id>/info
    
    auth_process_count = 1 << 6
} auth_process_actions;

typedef enum {
    auth_resource_process
} auth_resource_types;

typedef union {
    struct {
        u64 auth_type: 8;
        u64 rsvd: 24;
        u64 action_type: 32;
    };
    u64 entitlement;
} auth_entitlement_t;

auth_token auth_get_proc_token(process_t *t);

bool auth_valid(auth_token owner, u32 type, auth_resource resource_id);

auth_resource auth_map_process(process_t *p);

bool auth_request(auth_token owner, u32 type, auth_resource resource_id);

bool auth_entitlement(auth_token owner, u32 type, auth_resource_types resource_type);