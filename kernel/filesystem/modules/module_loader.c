#include "module_loader.h"
#include "console/kio.h"
#include "string/string.h"
#include "sysregs.h"
#include "memory/page_allocator.h"
#include "syscalls/syscalls.h"
#include "files/dir_list.h"
#include "exceptions/exception_handler.h"
#include "files/vfs.h"

#define MODULE_STRICT

bool root_stat(const char *path, fs_stat *out_stat){
    if (!out_stat) return false;
    out_stat->size = 0;
    out_stat->type = entry_directory;
    return true;
}

system_module root_module = {
    .name = "root",
    .mount = "/",
    .getstat = root_stat,
    .readdir = 0,
};

bool load_module_to(hash_map_t* modules, system_module *module){
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
    if (!module->init(module)){
        if (strcmp(module->mount,"/console")) kprintf("[MODULE] failed to load module %s. Init failed",module->name);
        return false;
    }
    hash_map_put_dictionary(modules, module->mount, PHYS_TO_VIRT_P(module));
    return true;
}

bool unload_module_from(hash_map_t* modules, system_module *module){
    if (!modules) return false;
    if (!module->init) return false;
    if (module->fini) module->fini();
    hash_map_remove(modules, module->mount, strlen(module->mount), 0);
    return false;
}

system_module* get_module_from(hash_map_t* modules, const char **full_path){
    if (!modules) return 0;
    if (!full_path || !*full_path) return 0;
    const char *path = *full_path;
    if (!strlen(path)) return 0;
    if (*path == '/'){ 
        path++;
        *full_path += 1;
    }
    string_slice mod_name = first_path_component(path);
    if (!mod_name.length){
        return &root_module;
    }
    if (mod_name.data[0] == '/'){
        mod_name.data++;
        mod_name.length--;
        *full_path += 1;
    }
    *full_path += mod_name.length;
    if (!mod_name.length){
        return &root_module;
    }
    return hash_map_get(modules, mod_name.data, mod_name.length);
}

static u64 index = 0, count = 0;
static uint64_t *list_offset;

static fs_dir_list_helper *dir_helper;

void iterate_root(void* key, u64 keylen, void* value){
    count++;
    if (count <= index) return;
    
    system_module *mod = value;
    if (!dir_list_fill(dir_helper, mod->mount)){
        if (list_offset) *list_offset = index;
        return;
    }
}

size_t list_root_from(hash_map_t* modules, fs_dir_list_helper *helper, uint64_t *offset){
    
    dir_helper = helper;
    index = offset ? *offset : 0;
    count = 0;
    
    hash_map_for_each(modules, iterate_root);
    
    return dir_buf_size(helper);
}
