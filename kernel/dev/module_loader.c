#include "module_loader.h"
#include "console/kio.h"
#include "string/string.h"
#include "sysregs.h"
#include "memory/page_allocator.h"
#include "syscalls/syscalls.h"

//TODO: use hashmaps
linked_list_t* modules;
void *mod_page = 0;

void* mod_alloc(size_t size){ 
    if (!mod_page) mod_page = page_alloc(PAGE_SIZE);
    return allocate(mod_page, size, page_alloc);
}

bool load_module(system_module *module){
    if (!modules){   
        modules = linked_list_create_alloc(mod_alloc, release);
    }
    if (!module->init){
        if (strcmp(module->mount,"/console")) kprintf("[MODULE] module not initialized due to missing initializer");//TODO: can we make printf silently fail so logging becomes easier?
        return false;
    }
    if (!module->init()){
        if (strcmp(module->mount,"/console")) kprintf("[MODULE] failed to load module %s. Init failed",module->name);
        return false;
    }
    linked_list_push_front(modules, PHYS_TO_VIRT_P(module));
    return true;
}

int fs_search(void *node, void *key){
    system_module* module = (system_module*)node;
    if (!module) return -1;
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
    linked_list_node_t *node = linked_list_find(modules, (void*)&name, fs_search);
    linked_list_remove(modules, node);
    return false;
}

system_module* get_module(const char **full_path){
    linked_list_node_t *node = linked_list_find(modules, (void*)full_path, fs_search);
    return node ? ((system_module*)node->data) : 0;
}
