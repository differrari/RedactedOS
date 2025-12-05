#include "module_loader.h"
#include "console/kio.h"
#include "filesystem/filesystem.h"
#include "sysregs.h"

//TODO: use hashmaps
clinkedlist_t* modules;

bool load_module(system_module *module){
    if (!modules) modules = clinkedlist_create();
    clinkedlist_push_front(modules, PHYS_TO_VIRT_P(module));
    return module->init();
}

bool unload_module(system_module *module){
    return false;
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

system_module* get_module(const char **full_path){
    clinkedlist_node_t *node = clinkedlist_find(modules, (void*)full_path, fs_search);
    return node ? ((system_module*)node->data) : 0;
}
