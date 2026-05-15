#include "isolated_fs.h"
#include "filesystem/modules/fs_isolation.h"
#include "filesystem/modules/module_loader.h"
#include "environment.h"

char *bundle_redirect = 0;

bool resources_init(system_module *module){
    module->alias_info.alias_path = string_format("%s/resources",bundle_redirect);
    return true;
}

system_module bundle_module = {
    .name = "resources",
    .mount = "resources",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = resources_init,
};

bool shared_init(system_module *module){
    module->alias_info.alias_path = string_from_literal("/home");
    return true;
}

system_module shared_module = {
    .name = "shared",
    .mount = "shared",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = shared_init,
};

static u16 procid;

bool env_init(system_module *module){
    if (!procid) return false;
    register_environment(procid);
    module->alias_info.alias_path = string_format("/environments/%i",procid);
    return true;
}

system_module env_module = {
    .name = "environment",
    .mount = "environment",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = env_init,
    .fini = 0,//TODO: These modules need to be unloaded too
};

void make_process_fs(process_t* proc, char *bundle){
    proc->permissions.fs_id = register_fs_id();
    module_root *root = get_fs_for_id(proc->permissions.fs_id);
    bundle_redirect = bundle;
    procid = proc->id;
    load_module_to(root, &bundle_module);
    load_module_to(root, &shared_module);
    load_module_to(root, &env_module);
}