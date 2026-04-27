#include "language.h"
#include "files/vfs.h"

#define DATA_SYNTAX DATA_SIGNATURE("CODESNTX")
#define DATA_FORMAT DATA_SIGNATURE("CODEFMT")
#define DATA_AUTOCOMPLETE DATA_SIGNATURE("CODECMPL")
#define DATA_TEMPLATE DATA_SIGNATURE("CODETMPL")

FS_RESULT syntax_highlight_open(const char *path, file *fd){
    string_slice first = first_path_component(!path || !strlen(path) ? DIR_AS_FILE : path+1);
    module_file *mfile = eval_entry(first);
    if (!mfile) return FS_RESULT_NOTFOUND;
    fd->id = mfile->fid;
    fd->size = 0x1000;
    fd->data_type = DATA_SYNTAX;
    fd->cursor = 0;
    return FS_RESULT_SUCCESS;
}

bool syntax_highlight_stat(const char *path, fs_stat *fstat){
    string_slice first = first_path_component(!path || !strlen(path) ? DIR_AS_FILE : path+1);
    module_file *mfile = eval_entry(first);
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
        size_t written = buffer_write_lim(&write_buf,(char*)&(text_format){ .color = 0xFFfcba03, .bounds = { i, 8 }}, sizeof(text_format));
        total += written;
        if (written != sizeof(text_format)) return total;
    }
    
    return total;
}

bool lfsp_init(){
    make_complex_entry("src", backing_transform, entry_directory, DATA_SIG_RAW, (file_actions){}, string_from_literal("/home/projects/code/braincode"));
    make_complex_entry("syntax", backing_transform, entry_directory, DATA_SYNTAX, (file_actions){
        .open = syntax_highlight_open,
        .read = syntax_highlight,
        .getstat = syntax_highlight_stat,
    }, string_from_literal("/language/src"));
    make_complex_entry("format", backing_transform, entry_directory, DATA_FORMAT, (file_actions){}, string_from_literal("/language/src"));
    make_complex_entry("complete", backing_transform, entry_directory, DATA_AUTOCOMPLETE, (file_actions){}, string_from_literal("/language/src"));
    make_complex_entry("template", backing_transform, entry_directory, DATA_TEMPLATE, (file_actions){}, string_from_literal("/language/src"));
    return true;
}

FS_RESULT lfsp_open(const char *path, file *descriptor){
    return FS_RESULT_DRIVER_ERROR;
}

system_module language_mod = {
    .name = "language support filesystem protocol",
    .mount = "language",
    .version = VERSION_NUM(0, 1, 0, 0),
    .init = lfsp_init,
    .open = vfs_open,
    .close = vfs_close,
    .read = vfs_read,
    .write = vfs_write,
    .getstat = vfs_stat,
    .readdir = vfs_readdir,
};