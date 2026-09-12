#include "files/system_module.h"
#include "files/vfs.h"
#include "files/helpers.h"

void tools_handle_entry(const char *directory, const char *file) {
    print("Found tool %s",file);
    if (strlen(file) && *file == '.') return;
    string fullpath = string_format("%s/%s",directory, file);
    make_complex_entry(file, backing_virtual, entry_file, DATA_SIG_REDPKG, (file_actions){}, fullpath);
}

void refresh_tools(){
    if (!entries) entries = stack_create(sizeof(module_file),32);
    size_t count = stack_count(entries);
    for (size_t i = 0; i < count; i++){
        string_free(STACK_GET(module_file, entries, i).name);
        string_free(STACK_GET(module_file, entries, i).alias_info.alias_path);
    }
    stack_reset(entries);
    traverse_directory("/home/tools", false, tools_handle_entry);
    traverse_directory("/boot/redos/tools", false, tools_handle_entry);
    traverse_directory("/ktools", false, tools_handle_entry);
}

system_module tools_mod = {
    .name = "tools folder",
    .mount = "tools",
    //TODO: can init be brought back now?
    .version = VERSION_NUM(0, 1, 0, 0),
    .open =     vfs_trace_open,
    .read =     vfs_trace_read,
    .write =    vfs_trace_write,
    .getstat =  vfs_trace_stat,
    .readdir =  vfs_trace_readdir,
};