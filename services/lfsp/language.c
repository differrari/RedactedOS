#include "language.h"
#include "files/folderfs.h"
#include "draw/textdraw.h"
#include "alloc/alloc.h"

#define DATA_SYNTAX DATA_SIGNATURE("CODESNTX")
#define DATA_FORMAT DATA_SIGNATURE("CODEFMT")
#define DATA_AUTOCOMPLETE DATA_SIGNATURE("CODECMPL")
#define DATA_TEMPLATE DATA_SIGNATURE("CODETMPL")

FS_RESULT syntax_highlight_open(const char *path, file *fd){
    if (path && *path == '/') path++;
    if (!path || !strlen(path)) path = DIR_AS_FILE;
    path_resolution resolution = parse_path(entries, path, path_resolution_forward, 0);
    module_file *mfile = resolution.file;
    if (!mfile) return FS_RESULT_NOTFOUND;
    
    if (!mfile->alias_info.alias_path.length){
        print("syntax highlighter should be aliased onto src");
        return FS_RESULT_SUCCESS;
    }
    
    vfs_open_aliased(mfile, resolution, fd);
    
    fd->id = mfile->fid;
    fd->size = 0x1000;
    fd->data_type = DATA_SYNTAX;
    fd->cursor = 0;
    return FS_RESULT_SUCCESS;
}

bool syntax_highlight_stat(const char *path, fs_stat *fstat){
    if (!fstat) return false;
    if (path && *path == '/') path++;
    if (!path || !strlen(path)) path = DIR_AS_FILE;
    path_resolution resolution = parse_path(entries, path, path_resolution_forward, 0);
    module_file *mfile = resolution.file;
    if (!mfile) return false;
    fstat->data_type = DATA_SYNTAX;
    fstat->type = mfile->entry_type;
    fstat->size = mfile->entry_type == entry_file ? 0x1000 : 0;
    return true;
}

size_t syntax_highlight(file *fd, char *buf, size_t size, file_offset off){
    module_file *mfile = find_entry(fd->id);
    if (!mfile->alias_info.alias_fd.id) {
        print("File should be aliased onto code");
        return 0;
    }
    char *contents = zalloc(mfile->alias_info.alias_fd.size);
    readf(&mfile->alias_info.alias_fd, contents, mfile->alias_info.alias_fd.size);
    
    buffer write_buf = (buffer){.buffer = buf, .buffer_size = size, .limit = size};
    
    size_t total = 0;
    for (size_t i = 0; i < size; i++){
        char *instance = memmem(contents + i, size - i, "#include", 8);
        if (!instance) break;
        size_t new_i = instance - contents;
        i = new_i;
        size_t written = buffer_write_lim(&write_buf,(char*)&(text_format){ .foreground = 0xFFfcba03, .bounds = { i, 8 }}, sizeof(text_format));
        total += written;
        if (written != sizeof(text_format)) return total;
    }
    
    return total;
}

typedef struct {
    string project_dir;
} lfsp_data;

buffer lfsp_hardcoded;
buffer lfsp_hardcoded_syntax;

char *lfsp_test = "(print (add 1 1))";

typedef enum {
    lfsp_src,
    lfsp_syntax
} lfsp_file_type;

buffer* lfsp_resolve_fd(file *fd){
    lfsp_file_type type = fd->id >> 32;
    if (!lfsp_hardcoded.buffer){
        lfsp_hardcoded = buffer_create(0x100, 0);
        buffer_write_const(&lfsp_hardcoded, lfsp_test);
    }
    if (!lfsp_hardcoded_syntax.buffer){
        lfsp_hardcoded_syntax = buffer_create(0x100, 0);
        // buffer_write_const(&lfsp_hardcoded_syntax, lfsp_test);
    }
    // u32 id = fd->id & UINT32_MAX;
    switch (type) {
        case lfsp_src: return &lfsp_hardcoded;
        case lfsp_syntax: return &lfsp_hardcoded_syntax;
    }
    return 0;
}

FS_RESULT lfsp_open(u64 id, string_slice file_name, file *fd){
    if (slice_lit_match(file_name, "src", true)){
        fd->id = ((u64)lfsp_src << 32) | (id & UINT32_MAX);
        fd->size = strlen(lfsp_test);
    }
    if (slice_lit_match(file_name, "syntax", true)){
        fd->id = ((u64)lfsp_syntax << 32) | (id & UINT32_MAX);
        fd->size = strlen(lfsp_test);
    }
    return FS_RESULT_SUCCESS;
}

static inline FS_RESULT lfsp_main_open(const char *path, file *fd){
    if (strncmp(path,"/new", 4) == 0){
        print("Creating new LFSP project");
        lfsp_data *data = new(lfsp_data);
        return folderfs_create_folder(data) == -1 ? FS_RESULT_DRIVER_ERROR : FS_RESULT_SUCCESS;
    }
    return folderfs_open(path, fd);
}

bool lfsp_init(){
    
    if (!folderfs_init()) return false;
    
    folderfs_resolve_fd = lfsp_resolve_fd;
    folderfs_custom_open = lfsp_open;
    
    static_entries += make_entry("new", backing_virtual, entry_file, (data_signature){}, (buffer){}) != 0;
    static_entries += make_entry(":id/src", backing_transform, entry_file, DATA_SIG_RAW, (buffer){}) != 0;
    static_entries += make_entry(":id/syntax", backing_transform, entry_file, DATA_SIG_RAW, (buffer){}) != 0;
    // make_complex_entry("syntax", backing_transform, entry_directory, DATA_SYNTAX, (file_actions){
    //     .open = syntax_highlight_open,
    //     .read = syntax_highlight,
    //     .getstat = syntax_highlight_stat,
    // }, string_from_literal("/lfsp/src"));
    // make_complex_entry("format", backing_transform, entry_directory, DATA_FORMAT, (file_actions){}, string_from_literal("/lfsp/src"));
    // make_complex_entry("complete", backing_transform, entry_directory, DATA_AUTOCOMPLETE, (file_actions){}, string_from_literal("/lfsp/src"));
    // make_complex_entry("template", backing_transform, entry_directory, DATA_TEMPLATE, (file_actions){}, string_from_literal("/lfsp/src"));
    return true;
}

system_module language_mod = {
    .name = "language support filesystem protocol",
    .mount = "lfsp",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = lfsp_init,
    .open = lfsp_main_open,
    // .close = folderfs_close,
    .read = folderfs_read,
    .write = folderfs_write,
    .getstat = folderfs_stat,
    .readdir = folderfs_readdir,
};

// /lfsp/new
// /lfsp/1/src