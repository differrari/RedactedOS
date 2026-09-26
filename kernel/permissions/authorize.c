#include "authorize.h"
#include "process/process.h"
#include "alloc/alloc.h"

#define auth_fail { print("[AUTH error] Token %i is not in the %i'd files for resource %i. This incident will be reported.",owner,type,resource_id.id); return false; }

#define AUTH_DEBUG
#ifdef AUTH_DEBUG
#define auth_print(...) print(__VA_ARGS__)
#else
#define auth_print(...)
#endif

hash_map_t *auth_entries;

static u32 auth_id_counter = 1;

auth_token auth_new_token(){
    return auth_id_counter++;
}

static u64 resource_id_counter = 1;

auth_token auth_get_proc_token(process_t *t){
    if (!t) return (auth_token){};
    if (!t->permissions.auth_id){
        t->permissions.auth_id = auth_new_token();
        auth_print("[AUTH] token %i assigned to process %i",t->permissions.auth_id,t->id);
    }
    return t->permissions.auth_id;
}

#define U56_MAX 0xFFFFFFFFFFFFFF

auth_resource auth_new_rid(auth_resource_types type){
    if (type > 255) return (auth_resource){};
    if (resource_id_counter >= U56_MAX) return (auth_resource){};
    return (auth_resource){
        .type = type & 0xFF,
        .id = resource_id_counter++
    };
}

auth_resource auth_map_process(process_t *p){
    if (!p) return (auth_resource){};
    if (!p->resource_id.id){
        p->resource_id = auth_new_rid(auth_resource_process);
        auth_print("[AUTH] resource id %i assigned to process %i",p->resource_id.id,p->id);
    }
    return p->resource_id;
}

bool auth_valid_proc(auth_token owner, auth_process_actions type, auth_resource resource_id){
    hash_map_t *resource_entries = hash_map_get(auth_entries, &resource_id, sizeof(auth_resource));
    if (!resource_entries) auth_fail;
    auth_entry *entry = hash_map_get(resource_entries, &owner, sizeof(auth_token));
    if (!entry) auth_fail;
    return (entry->actions & type) == type;
}

bool auth_valid(auth_token owner, u32 type, auth_resource resource_id){
    if (resource_id.type == auth_resource_process)
        return auth_valid_proc(owner, type, resource_id);
    auth_print("[AUTH error] Only process resource ids are allowed");
    return true;
}

bool request_auth_proc(auth_token owner, auth_process_actions type, auth_resource resource_id){
    auth_print("[AUTH debug] Token %i requesting resource %i for action %i",owner,resource_id.id, type);
    if (type >= auth_process_count) return false;
    hash_map_t *resource_entries = hash_map_get(auth_entries, &resource_id, sizeof(auth_resource));
    if (!resource_entries) {
        resource_entries = hash_map_create_alloc(64, alloc, release);
        hash_map_put(auth_entries, &resource_id, sizeof(auth_resource), resource_entries);
    }
    if (!resource_entries) return false;
    auth_entry *entry = hash_map_get(resource_entries, &owner, sizeof(auth_token));
    if (!entry){
        entry = new(auth_entry);
        hash_map_put(resource_entries, &owner, sizeof(auth_token), entry);
    }
    if (!entry) return false;
    entry->auth_token = owner;
    entry->actions |= type;
    auth_print("[AUTH debug] Token %i authorized for item %i on resource %i",owner, type, resource_id.id);
    return true;
}

bool auth_request(auth_token owner, u32 type, auth_resource resource_id){
    if (!auth_entries) auth_entries = hash_map_create_alloc(64,alloc,release);
    if (resource_id.type == auth_resource_process)
        return request_auth_proc(owner, type, resource_id);
    return false;
}