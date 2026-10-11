#include "resources.h"
#include "process/process.h"
#define MOG_MODULE "RESOURCE"
#define MOG_LEVEL MOG_VERBOSE
#include "utils/mog.h"

static u64 resource_id_counter = 1;

#define U56_MAX 0xFFFFFFFFFFFFFF

hash_map_t *file_mappings;

resource resource_new_rid(resource_types type){
    if (type > 255) return (resource){};
    if (resource_id_counter >= U56_MAX) return (resource){};
    return (resource){
        .type = type & 0xFF,
        .id = resource_id_counter++
    };
}

resource resource_map_process(process_t *p){
    if (!p) return (resource){};
    if (!p->resource_id.id){
        p->resource_id = resource_new_rid(resource_process);
        mog(VERBOSE, "resource id %i assigned to process %i",p->resource_id.id,p->id);
    }
    return p->resource_id;
}

resource resource_map_file_id(system_module *mod, u64 id){
    mog(VERBOSE, "Mapping fd %llx from mod %llx",id, mod);
    if (!file_mappings) file_mappings = hash_map_create(256);
    hash_map_t *module_map = hash_map_get(file_mappings, mod, sizeof(uptr));
    if (!module_map){
        module_map = hash_map_create(64);
        mog(VERBOSE, "New module map %llx",module_map);
        hash_map_put(file_mappings, mod, sizeof(uptr), module_map);
    }
    if (!module_map) return (resource){};
    mog(VERBOSE, "Module map %llx. Fid %llx",module_map,id);
    uptr resourceid = (u64)hash_map_get(module_map, (void*)&id, sizeof(u64));
    resource r = { .resource = resourceid };
    if (r.type == resource_invalid){
        r = resource_new_rid(resource_file);
        mog(VERBOSE, "New resource %llx",r.resource);
        resourceid = r.resource;
        hash_map_put(module_map, (void*)&id, sizeof(u64), (void*)r.resource);
        mog(VERBOSE, "New resource id %llx for file %llx of module %llx",r.resource,id,mod);
    } else mog(VERBOSE, "Existing resource id %llx for file %llx of module %llx",r.resource,id,mod);
    return r;
}