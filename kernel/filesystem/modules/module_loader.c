#include "module_loader.h"
#include "console/kio.h"
#include "string/string.h"
#include "sysregs.h"
#include "memory/page_allocator.h"
#include "syscalls/syscalls.h"
#include "files/dir_list.h"
#include "exceptions/exception_handler.h"
#include "files/vfs.h"


hash_map_t* modules;
void *mod_page = 0;

#define MODULE_STRICT

void* mod_alloc(size_t size){ 
    if (!mod_page) mod_page = page_alloc(PAGE_SIZE);
    return allocate(mod_page, size, page_alloc);
}

bool load_module(system_module *module){
    if (!modules) modules = hash_map_create(64);
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
    hash_map_put_dictionary(modules, module->mount, PHYS_TO_VIRT_P(module));
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
    hash_map_remove(modules, module->mount, strlen(module->mount), 0);
    return false;
}

system_module* get_module(const char **full_path){
    if (!full_path || !*full_path) return 0;
    const char *path = *full_path;
    string_slice mod_name = first_path_component(path);
    if (!mod_name.length) return 0;
    if (mod_name.data[0] == '/'){
        mod_name.data++;
        mod_name.length--;
        *full_path += 1;
    }
    *full_path += mod_name.length;
    return hash_map_get(modules, mod_name.data, mod_name.length);
}

u64 index = 0, count = 0;
uint64_t *list_offset;

fs_dir_list_helper *dir_helper;

void iterate_root(void* key, u64 keylen, void* value){
    count++;
    if (count <= index) return;
    
    system_module *mod = value;
    if (!dir_list_fill(dir_helper, mod->mount)){
        if (list_offset) *list_offset = index;
        return;
    }
}

size_t list_root(void* buf, size_t size, uint64_t *offset){
    
    fs_dir_list_helper helper = create_dir_list_helper(buf, size);
    dir_helper = &helper;
    index = offset ? *offset : 0;
    
    hash_map_for_each(modules, iterate_root);
    
    return dir_buf_size(&helper);
}