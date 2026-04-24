#include "module_loader.h"
#include "console/kio.h"
#include "string/string.h"
#include "sysregs.h"
#include "memory/page_allocator.h"
#include "syscalls/syscalls.h"
#include "files/dir_list.h"
#include "exceptions/exception_handler.h"

//TODO: use hashmaps
linked_list_t* modules;
void *mod_page = 0;

#define MODULE_STRICT

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
    if (!module->version){
        string format = string_format("Version number cannot be null for module /%s",module->mount);
        if (strcmp(module->mount,"/console")) 
        #ifdef MODULE_STRICT
            panic(format.data,0);
        #else 
            kprintf(format.data);
        #endif
        string_free(format);
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
    int index = strstart_case(*path, module->mount, true);
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

size_t list_root(void* buf, size_t size, uint64_t *offset){
    
    fs_dir_list_helper helper = create_dir_list_helper(buf, size);
    
    u64 index = offset ? *offset : 0;
    
    linked_list_node_t *node = linked_list_get(modules, index);
    
    do {
        if (node && node->data){
            system_module *mod = node->data;
            if (!dir_list_fill(&helper, mod->mount)){
                if (offset) *offset = index;
                return dir_buf_size(&helper);
            }
            index++;
        }
        node = node->next;
    } while(node);
    
    return dir_buf_size(&helper);
}