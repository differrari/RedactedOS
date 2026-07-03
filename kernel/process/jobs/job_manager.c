#include "job_manager.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "data/struct/linked_list.h"

job_id_t job_id_counter = 1;

typedef struct {
    thread_t *requester;
    thread_t *worker;
    job_id_t id;
} job_state_t;

linked_list_t *job_list;

void *job_page;

void* job_man_alloc(size_t size){
    return allocate(job_page, size, page_alloc);
}

job_state_t* job_alloc(){
    if (!job_page) job_page = page_alloc(PAGE_SIZE);
    if (!job_list) job_list = linked_list_create_alloc(job_man_alloc, release);
    job_state_t *job = job_man_alloc(sizeof(job_state_t));
    job->id = job_id_counter++;
    linked_list_push(job_list, job);
    return job;
}

job_id_t create_new_job(job_application_t application){
    job_state_t *job = job_alloc();
    process_t *requesting_proc = get_proc_by_pid(application.requesting_pid);
    if (!requesting_proc){
        print("[JOB error] Unknown requesting proc %i",application.requesting_pid);
    }
    thread_t *requester = (thread_t*)get_thread_from_proc(requesting_proc, application.requesting_tid);
    job->requester = requester;
    process_t *fs_owner = get_proc_by_pid(application.worker_pid);
    print("[JOB DEBUG] Sync between %i - %i will happen with job %i",requester->pid,application.worker_pid,job->id);
    thread_t *new_t = alloc_thread();
    new_thread(fs_owner, new_t, fs_owner->spsr, (uptr)fs_owner->exposed_fs.getstat);
    new_t->job_id = job->id;
    job->worker = new_t;
    requester->thread_state = BLOCKED;
    schedule_thread(fs_owner, new_t);
    return job->id;
}

job_state_t* get_job_state(job_id_t job_id){
    for (linked_list_node_t *cur = job_list->head; cur && cur->data; cur = cur->next){
        job_state_t *job = cur->data;
        if (job->id == job_id) return job;
    }
    return 0;
}

void fulfill_job(job_id_t job_id, thread_t *thread){
    job_state_t *st = get_job_state(job_id);
    if (!st) {
        print("[JOB error] Could not find id %i",job_id);
        return;
    }
    if (thread->job_id != job_id || st->worker != thread){
        print("[JOB error] termination request by wrong thread %i",thread->tid);
        return;
    }
    st->requester->thread_state = READY;//TODO: schedule once the scheduler is fully thread-based
    print("[JOB] %i fulfilled",job_id);
}