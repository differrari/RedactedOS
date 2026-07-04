#pragma once

#include "files/jobs.h"
#include "process/process.h"

static inline void job_application_append_buffer(job_application_t *app, job_buffer buf){
    if (app->buffer_count >= MAX_JOB_BUFFERS) return;
    app->buffers[app->buffer_count++] = buf;
}

static inline void job_serialize_str(job_application_t *application, u8 arg_num, const char *str){
    job_buffer buf = {
        .ptr = {.ptr = (uptr)str, .size = strlen(str)+1},
        .arg_num = arg_num,
        .explicit_size = false
    };
    job_application_append_buffer(application, buf);
}

static inline void job_serialize_stat(job_application_t *application, int arg_num, fs_stat *stat){
    job_buffer buf = {
        .ptr = {.ptr = (uptr)stat, .size = sizeof(fs_stat)},
        .arg_num = arg_num,
        .explicit_size = false
    };
    job_application_append_buffer(application, buf);
}

job_id_t create_new_job(job_application_t application);
void fulfill_job(job_id_t job_id, thread_t *thread);
