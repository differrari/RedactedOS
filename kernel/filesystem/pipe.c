#include "pipe.h"
#include "filesystem.h"
#include "memory/page_allocator.h"
#include "data_struct/hashmap.h"
#include "data_struct/linked_list.h"
#include "process/scheduler.h"

static void *pipe_page;
chashmap_t *pipe_map;

//TODO: options
FS_RESULT create_pipe(const char *source, const char* destination, PIPE_OPTIONS options, file *out_fd){
    if (!pipe_page) pipe_page = palloc(PAGE_SIZE, MEM_PRIV_KERNEL, MEM_RW, false);
    pipe_t *pipe = (pipe_t*)kalloc(pipe_page, sizeof(pipe_t), ALIGN_16B, MEM_PRIV_KERNEL);
    pipe->pid = get_current_proc_pid();
    system_module *mod = 0;
    FS_RESULT result = open_file_global(source, &pipe->write_fd, &mod);
    if (result == FS_RESULT_SUCCESS){
        result = open_file(destination, &pipe->read_fd);
        if (result == FS_RESULT_SUCCESS){
            if (!pipe_map) pipe_map = chashmap_create(64);
            clinkedlist_t *list = chashmap_get(pipe_map, &pipe->write_fd.id, sizeof(uint64_t));
            bool add_entry = false;
            if (!list){
                add_entry = true;
                list = clinkedlist_create();
            }
            clinkedlist_push_front(list, pipe);
            if (add_entry)
                chashmap_put(pipe_map, &pipe->write_fd.id, sizeof(uint64_t), list);
            out_fd->cursor = 0;
            out_fd->id = pipe->read_fd.id;
            out_fd->size = pipe->read_fd.size;
        }
    }
    if (result != FS_RESULT_SUCCESS) kfree(pipe, sizeof(pipe_t));
    return result;
}

FS_RESULT close_pipe(file *fd){
    return FS_RESULT_DRIVER_ERROR;
}

void update_pipes(uint64_t mfid, const char *buf, size_t size){
    if (!pipe_map) return;
    clinkedlist_t *list = chashmap_get(pipe_map, &mfid, sizeof(uint64_t));
    if (!list || !list->head) return;
    for (clinkedlist_node_t *head = list->head; head; head = head->next){
        if (!head->data) continue;
        pipe_t *pipe = (pipe_t*)head->data;
        if (!pipe) continue;
        write_file(&pipe->read_fd, buf, size);
    }
}

void close_pipes_for_process(uint16_t pid){

}
