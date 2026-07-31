#pragma once

#include "files/jobs.h"
#include "process/process.h"

//TODO: We can probably optimize current proc fs more, but right now it blocks the entire process so we can't allow this case
#define job_make(job_type,mod,action)\
process_t *owner_proc = get_proc_by_pid(mod->owner);\
if (!is_privileged(owner_proc)){\
    job_application_t app = (job_application_t){\
        .requesting_pid = get_current_proc()->id,\
        .requesting_tid = get_current_thread()->tid,\
        .worker_pid = owner_proc->id,\
        .type = job_type\
    };\
    action\
    create_new_job(app,mod);\
    switch_proc(YIELD);\
    return false;\
}

static inline void job_application_append_buffer(job_application_t *app, job_buffer buf){
    if (app->buffer_count >= MAX_JOB_BUFFERS) return;
    app->buffers[app->buffer_count++] = buf;
}

static inline void job_serialize_str(job_application_t *application, u8 arg_num, const char *str){
    size_t size = strlen(str)+1;
    job_buffer buf = {
        .worker_ptr = {.ptr = (uptr)str, .size = size},
        .orig_ptr = {.ptr = (uptr)str, .size = size},
        .sync = copy_on_start,
        .arg_num = arg_num,
        .explicit_size = false
    };
    job_application_append_buffer(application, buf);
}

static inline void job_serialize_buf(job_application_t *application, u8 arg_num, bool explicit_size, void *ptr, size_t size, job_sync_type sync_type){
    job_buffer buf = {
        .worker_ptr = {.ptr = (uptr)ptr, .size = size},
        .orig_ptr = {.ptr = (uptr)ptr, .size = size},
        .sync = sync_type,
        .arg_num = arg_num,
        .explicit_size = explicit_size
    };
    job_application_append_buffer(application, buf);
}

static inline void job_serialize_fd(job_application_t *application, u8 arg_num, file *fd, job_sync_type sync_type){
    job_buffer buf = {
        .worker_ptr = {.ptr = (uptr)fd, .size = sizeof(file)},
        .orig_ptr = {.ptr = (uptr)fd, .size = sizeof(file)},
        .sync = sync_type,
        .arg_num = arg_num,
        .explicit_size = false,
        .fd = true
    };
    job_application_append_buffer(application, buf);
}

static inline void job_serialize_stat(job_application_t *application, int arg_num, fs_stat *stat){
    job_buffer buf = {
        .worker_ptr = {.ptr = (uptr)stat, .size = sizeof(fs_stat) },
        .orig_ptr = {.ptr = (uptr)stat, .size = sizeof(fs_stat) },
        .sync = copy_on_end,
        .arg_num = arg_num,
        .explicit_size = false,
    };
    job_application_append_buffer(application, buf);
}

static inline void job_serialize_off(job_application_t *application, int arg_num, file_offset *offset){
    job_buffer buf = {
        .worker_ptr = {.ptr = (uptr)offset, .size = sizeof(file_offset *) },
        .orig_ptr = {.ptr = (uptr)offset, .size = sizeof(file_offset *) },
        .sync = copy_on_start | copy_on_end,
        .arg_num = arg_num,
        .explicit_size = false,
    };
    job_application_append_buffer(application, buf);
}

job_id_t create_new_job(job_application_t application, system_module *mod);
void fulfill_job(job_id_t job_id, u64 ret, thread_t *thread);
