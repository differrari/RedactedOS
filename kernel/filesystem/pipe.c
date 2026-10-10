#include "pipe.h"
#include "filesystem.h"
#include "memory/page_allocator.h"
#include "data/struct/hashmap.h"
#include "data/struct/linked_list.h"
#include "process/scheduler.h"
#define MOG_MODULE "Pipe"
#define MOG_LEVEL MOG_ALL
#include "utils/mog.h"

static void *pipe_page;
hash_map_t *pipe_map;

FS_RESULT create_pipe(module_root *src_root, const char *source, module_root *dst_root, const char* destination, pipe_options options, file *out_fd){
    if (!src_root) {
        mog(ERROR, "No source file filesystem root");
        return FS_RESULT_NOTFOUND;
    }
    if (!dst_root){
        mog(VERBOSE,"Using source module root as destination");
        dst_root = src_root;
    }
    if (!pipe_page) pipe_page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    pipe_t *pipe = (pipe_t*)kalloc(pipe_page, sizeof(pipe_t), ALIGN_16B, MEM_PRIV_KERNEL);
    pipe->pid = get_current_proc_pid();
    FS_RESULT result = open_file_global(src_root, source, &pipe->write_fd, &pipe->write_mod);
    if (result == FS_RESULT_SUCCESS){
        result = open_file(dst_root, destination, &pipe->read_fd);
        if (result == FS_RESULT_SUCCESS){
            if (!pipe_map) pipe_map = hash_map_create(64);
            linked_list_t *list = hash_map_get(pipe_map, &pipe->write_fd.id, sizeof(uint64_t));
            if (!list){
                list = linked_list_create();
                hash_map_put(pipe_map, &pipe->write_fd.id, sizeof(uint64_t), list);
            }
            linked_list_push_front(list, pipe);
            out_fd->cursor = 0;
            out_fd->id = pipe->read_fd.id;
            out_fd->size = pipe->read_fd.size;
            mog(VERBOSE, "Opened pipe between %s and %s with fd %i and src fd %x",source,destination,out_fd->id,pipe->write_fd.id);
            if (options & pipe_from_beginning){
                char *buf = zalloc(pipe->write_fd.size);
                size_t l = pipe->write_mod->read(&pipe->write_fd, buf, pipe->write_fd.size, 0);
                if (l){
                    write_file(&pipe->read_fd, buf, l);
                    mog(VERBOSE, "Piped %i of existing input to pipe output",l);
                }
            }
        } else mog(ERROR, "Failed to open destination of pipe");
    } else mog(ERROR, "Failed to open source of pipe %s",source);
    if (result != FS_RESULT_SUCCESS) kfree(pipe, sizeof(pipe_t));
    return result;
}

FS_RESULT close_pipe(file *fd){
    mog(IMPLEMENTATION_ERROR, "Closing pipes not yet supported");//TODO: Close pipes, the fd is not the hashmap index
    return FS_RESULT_DRIVER_ERROR;
}

void update_pipes(uint64_t mfid, const char *buf, size_t size){
    if (!pipe_map) return;
    linked_list_t *list = hash_map_get(pipe_map, &mfid, sizeof(uint64_t));
    if (!list || !list->head) return;
    for (linked_list_node_t *head = list->head; head; head = head->next){
        if (!head->data) continue;
        pipe_t *pipe = (pipe_t*)head->data;
        if (!pipe) continue;
        write_file(&pipe->read_fd, buf, size);
    }
}

static int32_t close_pid;

void close_pipe_list(void *key, uint64_t keylen, void *value){
    linked_list_t *list = (linked_list_t*)value;
    linked_list_node_t *prev = 0;
    for (linked_list_node_t *node = list->head; node; node = node->next){
        pipe_t *pipe = (pipe_t*)node->data;
        if (pipe->pid == close_pid){
            if (prev) prev->next = node->next;
            else list->head = node->next;

            close_file_global(&pipe->write_fd, pipe->write_mod);
            close_file(&pipe->read_fd);
            release(node->data);
            release(node);
        }
    }
}

void close_pipes_for_process(uint16_t pid){
    close_pid = pid;
    hash_map_for_each(pipe_map, close_pipe_list);
    close_pid = -1;
}
