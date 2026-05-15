#include "fs_isolation.h"
#include "data/struct/chunk_array.h"
#include "module_loader.h"
#include "console/kio.h"
#include "memory/memory.h"

chunk_array_t *fs_permissions;

u64 register_fs_id(){
    if (!fs_permissions) fs_permissions = chunk_array_create(sizeof(uptr), 256);
    hash_map_t *map = hash_map_create(64);
    return chunk_array_push(fs_permissions, &map);
}

hash_map_t* get_fs_for_id(u64 id){
    if (!fs_permissions) return 0;
    return *(hash_map_t**)chunk_array_get(fs_permissions, id);
}

hash_map_t* kernel_modules;

hash_map_t* kernel_fs(){
    return kernel_modules;
}

bool load_module(system_module *module){
    if (!kernel_modules) kernel_modules = hash_map_create(64);
    return load_module_to(kernel_modules, module);
}

bool unload_module(system_module *module){
    return unload_module_from(kernel_modules, module);
}

system_module* get_module(const char **full_path){
    return get_module_from(kernel_modules, full_path);
}

size_t list_root(void* buf, size_t size, uint64_t *offset){
    return list_root_from(kernel_modules, buf, size, offset);
}

string resolve_isolated_path(const char *path, u64 id, module_root *resolved){
    if (!path || !resolved) return (string){};
    hash_map_t *localfs = get_fs_for_id(id);
    const char *localpath = path;
    system_module *localmod = get_module_from(localfs, &localpath);
    if (!localmod){
        const char *rootpath = path;
        system_module *rootmod = get_module(&rootpath);
        if (!rootmod){
            return (string){};
        }
        memcpy(resolved,kernel_modules,sizeof(module_root));
        return string_from_literal(path);
    }
    if (localmod->alias_info.alias_path.length){
        string s = string_format("%S%s", localmod->alias_info.alias_path, localpath);
        const char *rootpath = s.data;
        system_module *rootmod = get_module(&rootpath);
        if (!rootmod) return (string){};
        memcpy(resolved,kernel_modules,sizeof(module_root));
        return s;
    }
    memcpy(resolved,localfs,sizeof(module_root));
    return string_from_literal(path);
}