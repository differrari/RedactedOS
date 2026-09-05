#include "isolated_fs.h"
#include "filesystem/modules/fs_isolation.h"
#include "filesystem/modules/module_loader.h"
#include "process/environment/environment.h"
#include "alloc/alloc.h"

bool stub_init(system_module *module){
    return true;
}

system_module bundle_module = {
    .name = "resources",
    .mount = "resources",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = stub_init,
};

bool shared_init(system_module *module){
    if (!module->alias_info.alias_path.length) module->alias_info.alias_path = string_from_literal("/home");
    return true;
}

system_module shared_module = {
    .name = "shared",
    .mount = "shared",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = shared_init,
};

bool docs_init(system_module *mod){
    if (!mod->alias_info.alias_path.length) mod->alias_info.alias_path = string_from_literal("/home/docs");
    return true;
}

system_module doc_module = {
    .name = "documentation",
    .mount = "docs",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = docs_init
};

system_module env_module = {
    .name = "environment",
    .mount = "environment",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = stub_init,
    .fini = 0,//TODO: These modules need to be unloaded too
};

static inline system_module* clone_mod(system_module *mod){
    system_module *m = new(system_module);
    memcpy(m, mod, sizeof(system_module));
    return m;
}

void make_process_fs(process_t* proc, char *bundle){
    proc->permissions.fs_id = register_fs_id();
    module_root *root = get_fs_for_id(proc->permissions.fs_id);
    
    if (bundle){
        system_module *local_bun = clone_mod(&bundle_module);
        local_bun->alias_info.alias_path = string_format("%s/resources",bundle);
        load_module_to(root, local_bun);
    }
    
    load_module_to(root, clone_mod(&shared_module));
    
    register_environment(proc->id);
    system_module *local_env = clone_mod(&env_module);
    local_env->alias_info.alias_path = string_format("/environments/%i",proc->id);
    load_module_to(root, local_env);
    
    load_module_to(root, clone_mod(&doc_module));
}

bool load_process_module(process_t *p, system_module *m, bool global){
    if (!p->permissions.owned_fs_id) p->permissions.owned_fs_id = register_fs_id();
    module_root *root = get_fs_for_id(p->permissions.fs_id);
    system_module *mod = zalloc(sizeof(system_module));
    memcpy(mod, m, sizeof(system_module));
    mod->name = string_from_literal(m->name).data;
    mod->mount = string_from_literal(m->mount).data;
    mod->owner = p->id;
    bool ret = load_module_to(root, mod);
    if (!ret) return false;
    if (!global) return true;
    return load_module(mod);
}