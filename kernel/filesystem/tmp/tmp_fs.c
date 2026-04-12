#include "tmp_fs.h"
#include "dev/driver_base.h"
#include "string/string.h"
#include "console/kio.h"
#include "files/dir_list.h"

//TODO: make stack type
#define MAX_TMP_ENTRIES 16
module_file entries[MAX_TMP_ENTRIES];
int entry_count = 0;

//TODO: move to system_module helper 
static inline bool make_entry(const char *name, fs_backing_type back_type, fs_entry_type ent_type, buffer buf){
    if (entry_count >= MAX_TMP_ENTRIES-1) return false;
    entries[entry_count++] = (module_file){
        .name = string_from_literal(name),
        .backing_type = back_type,
        .entry_type = ent_type,
        .file_buffer = buf,
        .references = 0,
        .read_only = false,
        .ignore_cursor = false,
        .fid = reserve_fd_gid(name),
    };
    return true;
}

static inline module_file* eval_entry(const char *path){
    for (int i = 0; i < entry_count; i++){
        if (strcmp(path, entries[i].name.data) == 0) return &entries[i];
    }
    return 0;
}

bool init_tmp(){
    make_entry("std", backing_virtual, entry_file, buffer_create(0x1000, buffer_opt_none));
    return true;
}

FS_RESULT tmp_open(const char *path, file *fd){
    module_file *mfile = eval_entry(path+1);
    kprintf("Mfile for %s = %llx",path,mfile);
    if (!mfile) return FS_RESULT_NOTFOUND;
    mfile->references++;
    return FS_RESULT_SUCCESS;
}

size_t tmp_read(file *fd, char* buf, size_t size, file_offset offset){
    module_file *mfile = (module_file*)&entries[0];
    if (!mfile) return 0;
    return buffer_read(&mfile->file_buffer, buf, size, offset);
}

size_t tmp_write(file *fd, const char* buf, size_t size, file_offset offset){
    module_file *mfile = (module_file*)&entries[0];
    if (!mfile) return 0;
    return buffer_write_to(&mfile->file_buffer, buf, size, fd->cursor);
}

bool tmp_stat(const char *path, fs_stat *out_stat){
    path = seek_to(path,'/');
    if (!strlen(path))
        return stat_dir(out_stat);
    module_file *file = eval_entry(path);
    if (file){
        out_stat->size = file->file_buffer.buffer_size;
        out_stat->type = file->entry_type;
        return true;
    }
    return false;
}

size_t tmp_list(const char *path, void *buf, size_t size, file_offset *offset){
    fs_dir_list_helper helper = create_dir_list_helper(buf, size);
    
    for (int i = offset ? *offset : 0; i < entry_count; i++){
        if (!dir_list_fill(&helper, entries[i].name.data)){
            if (offset) *offset = i;
            return dir_buf_size(&helper);
        }
    }
    
    return dir_buf_size(&helper);
}

system_module tmp_mod = {
    .name = "tmp",
    .mount = "tmp",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_tmp,
    .fini = 0,
    .open = tmp_open,
    .read = tmp_read,
    .write = tmp_write,
    .close = 0,
    .getstat = tmp_stat,
    .readdir = tmp_list,
};