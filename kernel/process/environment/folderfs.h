#pragma once

#include "types.h"
#define SKIP_ROUTER_FNS
#include "files/vfs.h"
#include "data/struct/linked_list.h"

static u64 static_entries = 0;
static linked_list_t *data_list; 

static inline bool folderfs_init(){
    static_entries += make_entry(":id", backing_virtual, entry_directory, 0, (buffer){}) != 0;
    return true;
}

static file_offset *out_offset;
static file_offset current_offset;

static void (*folderfs_custom_list)(void *ctx, u64 i, u64 *offset);

static inline void folderfs_list_ids(u64 *out_offset){
    if (!data_list) return;
    u64 i = 0;
    for (linked_list_node_t *node = data_list->head; node; node = node->next){
        if (node->data){
            if (folderfs_custom_list){
                print("Custom folder name");
                folderfs_custom_list(node->data, i, out_offset);
            } else {
                print("Default folder name %i",i);
                string s = string_format("%i",i);
                if (!dir_list_fill(router_fs_dir_helper, s.data)){
                    if (out_offset) *out_offset = current_offset;
                    string_free(s);
                    return;
                }
                string_free(s);
            }
        }
        i++;
    }
}

static void emit_route_contents(path_resolution resolution){
    if (!out_offset || *out_offset <= current_offset){
        if (slice_lit_match(resolution.path,resolution.file->name.data, true) && resolution.file->alias_info.alias_path.length){
            string fullpath = string_format("%S/%v",resolution.file->alias_info.alias_path,resolution.forwarded);
            dir_list(fullpath.data, router_fs_dir_helper->list, router_fs_dir_helper->limit, out_offset);
            return;
        }
        if (resolution.file->actions.readdir){
            resolution.file->actions.readdir(resolution.forwarded.data, router_fs_dir_helper->list, router_fs_dir_helper->limit, out_offset);
            return;
        }
        if (hash_map_size(resolution.params) && hash_map_get_dictionary(resolution.params, "id")){
            if (!strlen(hash_map_get_dictionary(resolution.params, "id"))){
                folderfs_list_ids(out_offset);
                return;
            }
            const char *lpc = seek_to(resolution.file->name.data,'/');
            if (!lpc || !strlen(lpc)) return;
            if (!dir_list_fill(router_fs_dir_helper, lpc)){
                if (out_offset) *out_offset = current_offset;
            }
            return;
        }
        if (!dir_list_fill(router_fs_dir_helper, resolution.file->name.data)){
            if (out_offset) *out_offset = current_offset;
        }
    }
    current_offset++;
}

static i64 folderfs_create_folder(void *ctx){
    if (!data_list) data_list = linked_list_create();
    linked_list_node_t *n = linked_list_push(data_list, ctx);
    if (!n) return -1;
    return linked_list_count(data_list);
}

static bool match_param(string_slice slice, string_slice param){
    if (slice_lit_match(param, ":id", true)){
        for (size_t i = 0; i < slice.length; i++) if (!is_digit(slice.data[i])) return false;
        return true;
    }
    return false;
}

static inline size_t folderfs_readdir(const char *path, void *buf, size_t size, file_offset *offset){
    fs_dir_list_helper helper = create_dir_list_helper(buf, size);
    out_offset = offset;
    return list_route_directory_contents(entries, path, &helper);
}

static FS_RESULT (*folderfs_custom_open)(u64 id, string_slice path, file *fd);

static inline FS_RESULT folderfs_open(const char *path, file *fd){
    if (!path || !strlen(path)) path = DIR_AS_FILE;
    path_resolution resolution = parse_path(entries, path, false, 0);
    if (!resolution.file){
        print("No file for %s",path);
        return FS_RESULT_NOTFOUND;
    }     
    if (!hash_map_size(resolution.params)){
        print("Resolution error for %s",path);
        return FS_RESULT_NOTFOUND;
    }
    string id_str = string_from_literal(hash_map_get_dictionary(resolution.params, "id"));
    hash_map_destroy(resolution.params);
    if (!id_str.length){
        print("Resolution error for %s",path);
        return FS_RESULT_NOTFOUND;
    } 
    i64 id = parse_int64(id_str.data, id_str.length);
    string_free(id_str);
    fd->cursor = 0;
    fd->data_type = resolution.file->data_type;
    if (id && folderfs_custom_open){
        const char *lpc = seek_to(resolution.file->name.data,'/');
        return folderfs_custom_open(id, slice_from_literal(lpc), fd);
    }
    fd->id = resolution.file->fid;
    fd->size = resolution.file->file_buffer.buffer_size;
    return FS_RESULT_SUCCESS;
}

buffer* (*folderfs_resolve_fd)(file *fd);

static inline size_t folderfs_read(file *fd,  char *buf, size_t size, file_offset offset){
    if (!folderfs_resolve_fd) return 0;
    return buffer_read(folderfs_resolve_fd(fd), buf, size, fd->cursor);
}

static inline size_t folderfs_write(file *fd, const char *buf, size_t size, file_offset offset){
    if (!folderfs_resolve_fd) return 0;
    size_t s = buffer_write_lim(folderfs_resolve_fd(fd), (void*)buf, size);
    print("Wrote %x out of %x to prop",s,size);
    return s;
}

bool folderfs_stat(const char *path, fs_stat *out_stat){
    if (!out_stat) return false;
    if (strlen(path) && *path == '/') path++;
    if (!strlen(path)){
        stat_dir(out_stat);
        return true;
    }
    path_resolution resolution = parse_path(entries, path, false, 0);
    if (!resolution.file){
        print("Failed to resolve file %s",path);
        return false;
    }
    out_stat->type = resolution.file->entry_type;
    out_stat->size = resolution.file->file_buffer.buffer_size;
    out_stat->data_type = resolution.file->data_type;
    return true;
}