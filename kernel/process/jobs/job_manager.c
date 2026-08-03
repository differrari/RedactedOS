#include "job_manager.h"
#include "process/process.h"
#include "process/scheduler.h"
#include "data/struct/linked_list.h"
#include "memory/mmu.h"
#include "memory/addr.h"
#include "memory/memory.h"
#include "filesystem/filesystem.h"

job_id_t job_id_counter = 1;

uptr job_kpec = 0;
sizedptr job_kstack = {};
extern int syscall_depth;
extern void job_restore_kernel();
extern uptr job_save_ret();

typedef struct {
    thread_t *requester;
    thread_t *worker;
    thread_t kernel_ctx;
    sizedptr kstack;
    job_id_t id;
    job_buffer buffers[8];
    size_t buffer_count;
    job_types type;
    system_module *mod;
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

bool prepare_thread(job_state_t *job, system_module *mod, job_application_t application, process_t *proc, thread_t *t){
    uptr entry = 0;
    switch (application.type){
        case job_stat: entry = (uptr)mod->getstat; break;
        case job_readdir: entry = (uptr)mod->readdir; break;
        case job_open: entry = (uptr)mod->open; break;
        case job_close: entry = (uptr)mod->close; break;
        case job_read: entry = (uptr)mod->read; break;
        case job_write: entry = (uptr)mod->write; break;
        case job_trunc: entry = (uptr)mod->truncate; break;
        default: return false;
    }
    if (!entry) return false;
    new_thread(proc, t, proc->spsr, entry);
    size_t total_size = 0;
    for (size_t i = 0; i < application.buffer_count; i++)
        total_size += application.buffers[i].worker_ptr.size;
    size_t num_pages = count_pages(total_size, PAGE_SIZE);
    uptr buffers = mm_alloc_mmap(&proc->mm, total_size, MEM_RW, VMA_KIND_SPECIAL, 0);
    uptr pbuffers = (uptr)palloc_inner(total_size, MEM_PRIV_SHARED, MEM_RW, true, false);
    for (size_t i = 0; i < num_pages; i++){
        mmu_map_4kb(proc->mm.ttbr0, buffers + (i * PAGE_SIZE), pbuffers + (i * PAGE_SIZE), MAIR_IDX_NORMAL, MEM_RW, MEM_PRIV_USER);
        register_proc_memory(PHYS_TO_VIRT(pbuffers + (i * PAGE_SIZE)), pbuffers + (i * PAGE_SIZE), MEM_RW, MEM_PRIV_KERNEL);
    }
    memset((void*)PHYS_TO_VIRT(pbuffers), 0, total_size);
    print("[JOB debug] buffers will go to %llx - %llx",buffers,pbuffers);
    uptr next_addr_pa = PHYS_TO_VIRT(pbuffers);
    uptr next_addr_va = buffers;
    
    for (size_t i = 0; i < application.buffer_count; i++){
        job_buffer buf = application.buffers[i];
        job->buffers[job->buffer_count++] = buf;
        if (buf.sync & copy_on_start && buf.worker_ptr.ptr)
            memcpy((void*)next_addr_pa, (void*)buf.worker_ptr.ptr, buf.worker_ptr.size);
        t->regs[buf.arg_num] = next_addr_va;
        job->buffers[i].worker_ptr.ptr = next_addr_va;
        if (buf.explicit_size)
            t->regs[buf.arg_num+1] = buf.worker_ptr.size;
        next_addr_pa += buf.worker_ptr.size;
        next_addr_va += buf.worker_ptr.size;
    }
    return true;
}

job_id_t create_new_job(job_application_t application, system_module *mod, thread_t *kthread){
    process_t *requesting_proc = get_proc_by_pid(application.requesting_pid);
    if (!requesting_proc){
        print("[JOB error] Unknown requesting proc %i",application.requesting_pid);
        return (job_id_t){};
    }
    job_state_t *job = job_alloc();
    job->type = application.type;
    job->mod = mod;
    thread_t *requester = (thread_t*)get_thread_from_proc(requesting_proc, application.requesting_tid);
    job->requester = requester;
    process_t *fs_owner = get_proc_by_pid(application.worker_pid);
    thread_t *new_t = alloc_thread();
    if (!prepare_thread(job, mod, application, fs_owner, new_t)){
        print("[JOB error] failed to prepare thread for job %i",job->id);
        return false;
    }
    print("[JOB debug] Sync between %i - %i will happen with job %i of type %i using thread %i",requester->pid,application.worker_pid,job->id,application.type,new_t->tid);
    new_t->job_id = job->id;
    job->worker = new_t;
    requester->state = BLOCKED;
    requesting_proc->state = BLOCKED;
    if (syscall_depth >= 1){
        print("[JOB debug] kstack has been saved to %llx - %x",job_kstack.ptr,job_kstack.size);
        memcpy(&job->kernel_ctx, kthread, sizeof(thread_t));
        for (u64 i = 0; i < sizeof(thread_t); i+=8){
            print("%i = %llx",i/8,((u64*)kthread)[i]);
        } 
        job->kernel_ctx.job_id = job->id;
        job->kernel_ctx.pc = job_save_ret();
        job->kstack = job_kstack;
    } else memset(&job->kernel_ctx, 0, sizeof(thread_t));
    schedule_thread(fs_owner, new_t);
    switch_proc(YIELD);
    return job->id;
}

job_state_t* get_job_state(job_id_t job_id){
    for (linked_list_node_t *cur = job_list->head; cur && cur->data; cur = cur->next){
        job_state_t *job = cur->data;
        if (job->id == job_id) return job;
    }
    return 0;
}

void* quick_translate(thread_t *thread, process_t *proc, uptr ptr){
    int status = 0;
    uptr addr = mmu_translate(proc->mm.ttbr0, ptr, &status);
    if (status){
        uint64_t esr = (0x24ULL << 26) | 0x7ULL;
        if (!mm_try_handle_page_fault(proc, thread, ptr, esr)) return 0;

        addr = mmu_translate((uint64_t*)proc->mm.ttbr0, ptr, &status);
        if (status) return 0;
    }
    return (void*)PHYS_TO_VIRT(addr);
}

extern uptr cpec;

static inline uptr translate_stack(uptr new_top, uptr ptr){
    return new_top-((uptr)ksp-ptr);
}

void fulfill_job(job_id_t job_id, u64 ret, thread_t *thread){
    job_state_t *st = get_job_state(job_id);
    if (!st) {
        print("[JOB error] Could not find id %i",job_id);
        return;
    }
    if (thread->job_id != job_id || st->worker != thread){
        print("[JOB error] termination request by wrong thread %i",thread->tid);
        return;
    }
    process_t *proc = get_proc_by_pid(st->requester->pid);
    for (size_t i = 0; i < st->buffer_count; i++){
        job_buffer buf = st->buffers[i];
        if (buf.sync & copy_on_end && buf.worker_ptr.ptr){
            print("[JOB debug] Copy buffer %x into %x",buf.worker_ptr.ptr,buf.orig_ptr.ptr);
            void* addr = quick_translate(st->requester, proc, buf.orig_ptr.ptr);
            if (!addr) continue;
            memcpy(addr, (void*)buf.worker_ptr.ptr, buf.worker_ptr.size);
            file *fd = addr;
            if (buf.fd){
                print("[JOB DEBUG] fd %i size %i signature %s",fd->id,fd->size,&fd->data_type);
                if (st->type == job_open){
                    instance_local_fd(st->mod, addr);
                }
            }
        }
    }
    print("[JOB] %i fulfilled by %i",job_id,thread->tid);
    if (st->kernel_ctx.job_id){
        //Restore kernel proc here and let it return to the process
        print("Restoring to kernel's functionality here %llx",st->kernel_ctx.pc);
        st->kernel_ctx.PROC_X0 = ret;
        job_kpec = (uptr)&st->kernel_ctx;
        cpec = (uptr)st->requester;
        // print("Kernel will restore to %i %i. with %llx of stack being restored to %llx from %llx",st->requester->pid,st->requester->tid,st->kstack.size,ksp,st->kstack.ptr);
        // print("memcpy(%llx,%llx,%llx)",ksp-st->kstack.size,st->kstack.ptr,st->kstack.size);
        // memcpy(ksp-st->kstack.size, (void*)st->kstack.ptr, st->kstack.size);


        // print("SP is %llx and LR is %llx",st->kernel_ctx.sp,st->kernel_ctx.regs[29]);

        st->kernel_ctx.sp = translate_stack((st->kstack.ptr+st->kstack.size), st->kernel_ctx.sp);
        print("Initial Address %llx",st->kernel_ctx.regs[29]);
        st->kernel_ctx.regs[29] = translate_stack((st->kstack.ptr+st->kstack.size), st->kernel_ctx.regs[29]);

        uptr addr = st->kernel_ctx.regs[29];
        uptr fp = 0;
        do {
            print("Address %llx",addr);
            fp = *(uptr*)addr;
            print("Link %llx",fp);
            fp = translate_stack(fp, st->kstack.ptr+st->kstack.size);
            print("In new stack %llx",fp);
            *(uptr*)addr = fp;
            addr = fp;
        } while(addr && (addr & 0xfffff00000000000) == 0xffffc00000000000);
        ready_thread(st->requester);
        job_restore_kernel();
        
        // st->kernel_ctx.sp = st->kstack.ptr-((uptr)ksp-st->kernel_ctx.sp);
        // job_restore_kernel();
    } else {
        ready_thread(st->requester);
        st->requester->PROC_X0 = ret;
    }
}