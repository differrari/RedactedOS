#include "module_loader.h"
#include "console/kio.h"
#include "filesystem/filesystem.h"

clinkedlist_t* modules;

bool load_module(driver_module module){
    if (!modules) modules = clinkedlist_create();
    clinkedlist_push_front(modules, (void*)&module);
    return module.init();
}

bool unload_module(driver_module module){
    return false;
}

int fs_search(void *node, void *key){
    driver_module* module = (driver_module*)node;
    const char** path = (const char**)key;
    kprintf("Comparing %s to %s",(uintptr_t)*path,(uintptr_t)module->mount);
    int index = strstart(*path, module->mount, false);
    if (index == (int)strlen(module->mount,0)){ 
        *path += index;
        return 0;
    }
    return -1;
}

driver_module* get_module(const char **full_path){
    kprintf("Seeking module %s",(uintptr_t)*full_path);
    clinkedlist_node_t *node = clinkedlist_find(modules, (void*)full_path, fs_search);
    return ((driver_module*)node->data);
}