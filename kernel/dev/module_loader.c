#include "module_loader.h"
#include "console/kio.h"
#include "filesystem/filesystem.h"
#include "sysregs.h"

//TODO: use hashmaps
clinkedlist_t* modules;

bool load_module(system_module *module){
    if (!modules) modules = clinkedlist_create();
    if (!module->init) return false;
    clinkedlist_push_front(modules, PHYS_TO_VIRT_P(module));
    return module->init();
}

int fs_search(void *node, void *key){
    system_module* module = (system_module*)node;
    const char** path = (const char**)key;
    int index = strstart_case(*path, module->mount,true);
    if (index == (int)strlen(module->mount)){ 
        *path += index;
        return 0;
    }
    return -1;
}

bool unload_module(system_module *module){
    if (!modules) return false;
    if (!module->init) return false;
    if (module->fini) module->fini();
    const char *name = module->mount;
    clinkedlist_node_t *node = clinkedlist_find(modules, (void*)&name, fs_search);
    clinkedlist_remove(modules, node);
    return false;
}

system_module* get_module(const char **full_path){
    clinkedlist_node_t *node = clinkedlist_find(modules, (void*)full_path, fs_search);
    return node ? ((system_module*)node->data) : 0;
}
