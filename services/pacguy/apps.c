#include "files/vfs.h"
#include "files/helpers.h"

void handle_entry(const char *directory, const char *file) {
    if (strlen(file) && *file == '.') return;
    string fullpath = string_format("%s/%s",directory, file);
    uint16_t ext_loc = find_find_extension_index((char*)file);
    string_slice name = make_string_slice(fullpath.data, fullpath.length - strlen(file), ext_loc);
    uint16_t extra = ext_loc ? 1 : 0;
    string_slice ext = make_string_slice(name.data + ext_loc + 1, 0, strlen(file)-ext_loc-extra);
    if (slice_lit_match(ext,"red",true)){
        print("Found file %s/%s",directory,file);
        make_complex_entry(file, backing_virtual, entry_directory, DATA_SIG_REDPKG, (file_actions){}, fullpath);
    } else string_free(fullpath);
}

void refresh_apps(){
    if (!entries) entries = stack_create(sizeof(module_file),32);
    size_t count = stack_count(entries);
    for (size_t i = 0; i < count; i++){
        string_free(STACK_GET(module_file, entries, i).name);
        string_free(STACK_GET(module_file, entries, i).alias_info.alias_path);
    }
    stack_reset(entries);
    traverse_directory("/home/applications", false, handle_entry);
    traverse_directory("/boot/redos/system", false, handle_entry);
}

system_module apps_mod = {
    .name = "apps folder",
    .mount = "apps",
    //TODO: can init be brought back now?
    .version = VERSION_NUM(0, 1, 0, 0),
    .open = vfs_open,
    .read = vfs_read,
    .write = vfs_write,
    .getstat = vfs_stat,
    .readdir = vfs_readdir,
};