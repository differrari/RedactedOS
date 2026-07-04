#include "job_manager.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "data/struct/linked_list.h"
#include "memory/mmu.h"
#include "memory/addr.h"
#include "memory/memory.h"

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

bool prepare_thread(job_application_t application, process_t *proc, thread_t *t){
    uptr entry = 0;
    switch (application.type){
        case job_stat: entry = (uptr)proc->exposed_fs.getstat; break;
        default: return false;
    }
    if (!entry) return false;
    new_thread(proc, t, proc->spsr, (uptr)proc->exposed_fs.getstat);
    size_t total_size = 0;
    for (size_t i = 0; i < application.buffer_count; i++)
        total_size += application.buffers[i].ptr.size;
    size_t num_pages = count_pages(total_size, PAGE_SIZE);
    uptr buffers = mm_alloc_mmap(&proc->mm, total_size, MEM_RW, VMA_KIND_SPECIAL, 0);
    uptr pbuffers = (uptr)palloc_inner(total_size, MEM_PRIV_SHARED, MEM_RW, true, false);
    for (size_t i = 0; i < num_pages; i++){
        mmu_map_4kb(proc->mm.ttbr0, buffers + (i * PAGE_SIZE), pbuffers + (i * PAGE_SIZE), MAIR_IDX_NORMAL, MEM_RW, MEM_PRIV_USER);
        register_proc_memory(PHYS_TO_VIRT(pbuffers + (i * PAGE_SIZE)), pbuffers + (i * PAGE_SIZE), MEM_RW, MEM_PRIV_KERNEL);
    }
    print("Our buffers will go to %llx - %llx",buffers,pbuffers);
    uptr next_addr_pa = PHYS_TO_VIRT(pbuffers);
    uptr next_addr_va = buffers;
    
    for (size_t i = 0; i < application.buffer_count; i++){
        job_buffer buf = application.buffers[i];
        memcpy((void*)next_addr_pa, (void*)buf.ptr.ptr, buf.ptr.size);
        t->regs[buf.arg_num] = next_addr_va;
        if (buf.explicit_size)
            t->regs[buf.arg_num+1] = buf.ptr.size;
        next_addr_pa += buf.ptr.size;
        next_addr_va += buf.ptr.size;
    }
    return true;
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
    if (!prepare_thread(application, fs_owner, new_t)) return false;
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