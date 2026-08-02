#include "scheduler.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "input/input_dispatch.h"
#include "exceptions/exception_handler.h"
#include "exceptions/timer.h"
#include "console/kconsole/kconsole.h"
#include "data/struct/hashmap.h"
#include "data/struct/queue.h"
#include "data/struct/linked_list.h"
#include "std/memory.h"
#include "memory/mmu.h"
#include "process/syscall.h"
#include "memory/addr.h"
#include "sysregs.h"
#include "filesystem/filesystem.h"
#include "filesystem/modules/module_loader.h"
#include "string/string.h"
#include "alloc/allocate.h"
#include "files/dir_list.h"
#include "stack_manager.h"
#include "graph/tres.h"

extern void save_pc_interrupt(uintptr_t ptr);
extern void restore_context(uintptr_t ptr);

void *proc_mem_page;

static inline void* proc_palloc(size_t s){
    return palloc(s, MEM_PRIV_KERNEL, MEM_RW, true);
}

static inline void* proc_alloc(size_t s){
    if (!proc_mem_page) proc_mem_page = proc_palloc(PAGE_SIZE);
    return allocate(proc_mem_page, s, proc_palloc);
}

thread_t* alloc_thread(){
    return proc_alloc(sizeof(thread_t));
}

static process_t *current_proc = 0;
static process_t *kernel_proc = 0;
static process_t *idle_proc = 0;
process_t *process_list = 0;
uint16_t proc_count = 0;
uint16_t next_proc_index = 1;

CQueue ready_queue = {};
linked_list_t sleeping_list = {};

extern hash_map_t *proc_opened_files;

__attribute__((noreturn)) void idle_entry() {
    for (;;) {
        asm volatile("dsb sy" ::: "memory");
        asm volatile("wfi");
    }
}

bool process_is_known(process_t *proc){
    if (!proc) return false;
    if (proc == idle_proc) return true;
    process_t *it = process_list;
    while (it) {
        if (it == proc) return true;
        it = it->process_next;
    }
    return false;
}

bool process_has_runtime_state(process_t *proc){
    return proc && (proc->main_thread.sp || proc->main_thread.pc || proc->main_thread.spsr || proc->main_thread.stack_info.size || proc->heap_phys || proc->mm.ttbr0 || proc->output || proc->alloc_map || proc->bundle || proc->code || proc->code_size || proc->va);
}

bool process_can_run(process_t *proc){
    if (!proc) return false;
    if (!process_is_known(proc) || proc->pending_reset) return false;
    if (proc->state == STOPPED || proc->suspended || !proc->main_thread.pc || !proc->main_thread.sp) return false;
    if (!is_privileged(proc)) return !!proc->mm.ttbr0;
    return !proc->mm.ttbr0;
}

bool process_can_reset(process_t *proc){
    return proc && proc->state == STOPPED && proc->pending_reset && !proc->procfs_refs;
}

void enqueue_ready_thread(thread_t *t){
    if (!t || t->pid == idle_proc->id || t->state == READY) return;
    if (!ready_queue.elem_size) cqueue_init(&ready_queue, 0, sizeof(thread_t*),0,0);
    t->state = READY;
    if (!cqueue_enqueue(&ready_queue, &t)) panic("ready enqueue failed", (t->pid << 16) | t->pid);
}

bool remove_sleeping_process(process_t *proc, uint16_t pid){
    bool removed = false;
    linked_list_node_t *sleep = sleeping_list.head;
    while (sleep) {
        linked_list_node_t *next = sleep->next;
        thread_t *thread = (thread_t*)sleep->data;
        if (thread && thread->pid == pid) {
            linked_list_remove(&sleeping_list, sleep);
            removed = true;
        }
        sleep = next;
    }
    return removed;
}

void save_return_address_interrupt(){
    save_pc_interrupt(cpec);
}

void update_sleep_timer() {
    if (sleeping_list.head) {
        thread_t *head_thread = (thread_t*)sleeping_list.head->data;
        if (head_thread) {
            uint64_t now = timer_now_msec();
            uint64_t wait = head_thread->wake_at_msec > now ? head_thread->wake_at_msec - now : 1;
            virtual_timer_reset(wait);
            virtual_timer_enable();
        } else virtual_timer_disable();
    } else virtual_timer_disable();
}

void switch_proc(ProcSwitchReason reason) {
    if (proc_count == 0)
        panic("No processes active", 0);
    thread_t *prev_t = (thread_t*)cpec;
    process_t *prev = current_proc, *next_proc = 0;
    if (prev && prev->state == RUNNING) {
        if (prev == idle_proc) prev->state = BLOCKED;
        else if (prev_t->state == RUNNING) enqueue_ready_thread(prev_t);
    }


    thread_t *next_thread = 0;
    while (!cqueue_is_empty(&ready_queue)) {
        thread_t *queued = 0;
        if (!cqueue_dequeue(&ready_queue, &queued)) break;
        if (!queued) continue;
        if (queued->state != READY) continue;
        process_t *proc = get_proc_by_pid(queued->pid);
        next_proc = (process_t*)proc;
        next_thread = queued;
        if (!process_can_run(next_proc)) continue;
        break;
    }

    if (!next_proc && current_proc && current_proc != idle_proc && current_proc->state == RUNNING && process_can_run(current_proc)){
        next_proc = current_proc;
        next_thread = (thread_t*)cpec;
    } 
    if (!next_proc || !process_can_run(next_proc)){
        next_proc = idle_proc;
        next_thread = &idle_proc->main_thread;
    }
    if (!next_proc || !process_can_run(next_proc)) panic("no runnable process", 0);
    
    if (!next_thread || next_thread->pid != next_proc->id) next_thread = &next_proc->main_thread;
    next_proc->state = RUNNING;
    next_thread->state = RUNNING;
    current_proc = next_proc;
    cpec = (uptr)next_thread;
    if (current_proc == idle_proc) timer_disable();
    else {
        timer_enable();
        timer_reset(current_proc->priority);
    }

    if (current_proc->mm.ttbr0) mmu_asid_ensure(&current_proc->mm);
    mmu_swap_ttbr(current_proc->mm.ttbr0 ? &current_proc->mm : 0);
    if (prev && prev != current_proc && prev != idle_proc && process_can_reset(prev)) reset_process(prev);

    process_restore();
}

void save_syscall_return(uint64_t value){
    if (!current_proc) return;
    get_current_thread()->PROC_X0 = value;
}

void process_restore(){
    if (!current_proc) panic("process_restore null process", 0);
    if (!process_is_known(current_proc)) panic("process_restore unknown process", cpec);
    if (current_proc->pending_reset || current_proc->state == STOPPED || !current_proc->main_thread.pc || !current_proc->main_thread.sp) {
        if (current_proc->mm.ttbr0) {
            current_proc->pending_reset = true;
            current_proc->state = STOPPED;
            switch_proc(HALT);
            panic("process_restore recovery returned", cpec);
        }
        panic("process_restore invalid process", cpec);
    }

    if (!is_privileged(current_proc)) {
        if (!current_proc->mm.ttbr0) panic("process_restore user process without ttbr0", current_proc->id);
        if (current_proc->main_thread.pc >= HIGH_VA) panic("user pc in kernel VA", current_proc->main_thread.pc);
        mmu_ttbr0_enable_user();
    } else mmu_ttbr0_disable_user(); 
    restore_context(cpec);
}

bool start_scheduler(){
    kprint("Starting scheduler");
    kconsole_clear();
    disable_interrupt();
    timer_init(current_proc ? current_proc->priority : PROC_PRIORITY_LOW);
    switch_proc(YIELD);
    return true;
}

bool init_scheduler(){
    load_module(&procfs_mod);
    return true;
}

uintptr_t get_current_heap(){
    if (current_proc->heap_phys) return (uintptr_t)dmap_pa_to_kva(current_proc->heap_phys);
    return current_proc->mm.mmap_bottom;
}

bool get_current_privilege(){
    return current_proc && is_privileged(current_proc);
}

process_t* get_current_proc(){
    return current_proc;
}

thread_t* get_current_thread(){
    return (thread_t*)cpec;
}

process_t* get_kernel_proc(){
    return kernel_proc;
}

process_t* get_idle_proc(){
    return idle_proc;
}

bool scheduler_in_idle(){
    return current_proc == idle_proc;
}

void ready_process(process_t *proc){
    irq_flags_t irq = irq_save_disable();
    if (!proc || !proc->id || proc->state == STOPPED || proc->pending_reset) {
        irq_restore(irq);
        return;
    }

    proc->spsr = proc->main_thread.spsr;
    
    enqueue_ready_thread(&proc->main_thread);
    proc->state = READY;
    irq_restore(irq);
}

void ready_thread(thread_t *t){
    irq_flags_t irq = irq_save_disable();
    if (!t || !t->pid || !t->tid || t->state == STOPPED || t->state == SLEEPING/* || proc->pending_reset*/) {
        irq_restore(irq);
        return;
    }
    
    enqueue_ready_thread(t);
    irq_restore(irq);
}

process_t* get_proc_by_pid(uint16_t pid){
    process_t *proc = process_list;
    while (proc) {
        if (proc->id == pid) return proc;
        proc = proc->process_next;
    }
    return NULL;
}

thread_t* get_thread_from_proc(process_t *proc, u16 tid){
    thread_t *t = &proc->main_thread;
    do {
        if (t->tid == tid) return t;
        t = t->next;
    } while (t);
    return 0;
}

uint16_t get_current_proc_pid(){
    return current_proc ? current_proc->id : 0;
}

void reset_process(process_t *proc){
    if (!proc) panic("reset_process null", 0);
    if (proc == current_proc) panic("reset_process current", proc->id);
    if (proc->procfs_refs) panic("reset_process with procfs refs", proc->id);

    uint16_t pid = proc->id;
    int32_t exit_code = proc->exit_code;
    bool counted = proc->main_thread.sp || proc->main_thread.pc || proc->main_thread.spsr || proc->main_thread.stack_info.size || proc->heap_phys || proc->mm.ttbr0;

    irq_flags_t irq = irq_save_disable();
    proc->pending_reset = false;
    proc->thread_count = 0;//TODO: clean up all threads
    proc->thread_ids = 0;

    remove_sleeping_process(proc, pid);

    update_sleep_timer();
    irq_restore(irq);
    proc->main_thread.sp = 0;
    proc->main_thread.pc = 0;
    proc->main_thread.spsr = 0;
    unmap_stack(proc, proc->main_thread.stack_info);
    memset(proc->main_thread.regs, 0, 31 * sizeof(proc->main_thread.regs[0]));
    memset(&proc->input_buffer, 0, sizeof(proc->input_buffer));
    memset(&proc->event_buffer, 0, sizeof(proc->event_buffer));
    proc->packet_buffer.read_index = 0;
    proc->packet_buffer.write_index = 0;
    for (int k = 0; k < PACKET_BUFFER_CAPACITY; k++){
        sizedptr p = proc->packet_buffer.entries[k];
        if (p.ptr)
            free_sizedptr(p);
        proc->packet_buffer.entries[k] = (sizedptr){0};
    }
    close_files_for_process(pid);

    if (proc->postmortem_output) {
        release((void*)proc->postmortem_output);
        proc->postmortem_output = 0;
        proc->postmortem_output_size = 0;
    }
    if (proc->output && proc->output_size) {
        void *snapshot = zalloc(proc->output_size+1);
        if (snapshot) {
            memcpy(snapshot, (void*)proc->output, proc->output_size);
            ((char*)snapshot)[proc->output_size] = 0;
            proc->postmortem_output = (kaddr_t)snapshot;
            proc->postmortem_output_size = proc->output_size;
        }
    }

    if (proc->debug_lines.ptr) {
        pfree((void*)proc->debug_lines.ptr, proc->debug_lines.size);
        proc->debug_lines = (sizedptr){0};
    }
    if (proc->debug_line_str.ptr) {
        pfree((void*)proc->debug_line_str.ptr, proc->debug_line_str.size);
        proc->debug_line_str = (sizedptr){0};
    }

    if (proc_opened_files) {
        //irq_flags_t irq = irq_save_disable();
        char proc_path[48] = {};
        string_format_buf(proc_path, sizeof(proc_path), "/%i/out", pid);
        uint64_t fid = reserve_fd_gid(proc_path);
        module_file *out_file = (module_file*)hash_map_get(proc_opened_files, &fid, sizeof(fid));
        if (out_file && (uintptr_t)out_file->file_buffer.buffer == (uintptr_t)proc->output) {
            size_t snapshot_size = proc->output_size;
            if (!snapshot_size) {
                out_file->buf = 0;
                out_file->file_buffer = (buffer){0};
                out_file->file_size = 0;
            } else {
                void *snapshot = zalloc(snapshot_size+1);
                if (snapshot) {
                    memcpy(snapshot, (void*)proc->output, snapshot_size);
                    ((char*)snapshot)[snapshot_size] = 0;
                    out_file->buf = (uptr)snapshot;
                    out_file->file_buffer = (buffer){
                        .buffer = snapshot,
                        .buffer_size = snapshot_size,
                        .limit = snapshot_size,
                        .options = buffer_opt_none,
                        .cursor = 0,
                    };
                    out_file->file_size = snapshot_size;
                } else {
                    out_file->buf = 0;
                    out_file->file_buffer = (buffer){0};
                    out_file->file_size = 0;
                }
            }
        }

        string_format_buf(proc_path, sizeof(proc_path), "/%i/state", pid);
        fid = reserve_fd_gid(proc_path);
        module_file *state_file = (module_file*)hash_map_get(proc_opened_files, &fid, sizeof(fid));
        if (state_file && (uintptr_t)state_file->file_buffer.buffer == (uintptr_t)&proc->state) {
            process_state *snapshot = (process_state*)zalloc(sizeof(proc->state));
            if (snapshot) {
                *snapshot = STOPPED;
                state_file->buf = (uptr)snapshot;
                state_file->file_buffer = (buffer){
                    .buffer = snapshot,
                    .buffer_size = sizeof(proc->state),
                    .limit = sizeof(proc->state),
                    .options = buffer_opt_none,
                    .cursor = 0,
                };
                state_file->file_size = sizeof(proc->state);
            } else {
                state_file->buf = 0;
                state_file->file_buffer = (buffer){0};
                state_file->file_size = 0;
            }
        }
        //irq_restore(irq);
    }

    if (proc->output) {
        pfree((void*)proc->output, PROC_OUT_BUF);
        proc->output = 0;
        proc->output_size = 0;
    }

    if (proc->mm.ttbr0) {
        for (uint16_t i = 0; i < proc->mm.vma_count; i++) {
            vma *m = &proc->mm.vmas[i];
            bool nofree = (m->flags & VMA_FLAG_NOFREE) != 0;
            uaddr_t start = m->start;
            uaddr_t end = m->end;
            if (m->kind == VMA_KIND_STACK) {
                if (!proc->mm.rss_stack_pages) continue;
                start = proc->mm.stack_commit;
                if (start < m->start) start = m->start;
                if (start >= end) continue;
            } else if (m->kind == VMA_KIND_ANON && !proc->mm.rss_anon_pages) continue;
            for (uaddr_t va = start; va < end; va += GRANULE_4KB) {
                paddr_t pa = 0;
                if (!mmu_unmap_and_get_pa((uint64_t*)proc->mm.ttbr0, (uint64_t)va, &pa)) continue;
                if (!nofree) pfree((void*)dmap_pa_to_kva(pa), GRANULE_4KB);
                if (m->kind == VMA_KIND_STACK) {
                    if (proc->mm.rss_stack_pages) proc->mm.rss_stack_pages--;
                } else if (m->kind == VMA_KIND_ANON) {
                    if (proc->mm.rss_anon_pages) proc->mm.rss_anon_pages--;
                }
            }
        }
        proc->mm.vma_count = 0;
    }

    if (proc->alloc_map) {
        if (proc->mm.ttbr0) {
            for (page_index *ind = proc->alloc_map; ind; ind = ind->header.next) ind->header.size = 0;
        }
        release_page_index(proc->alloc_map);
        proc->alloc_map = 0;
    }
    if (proc->mm.ttbr0) {
        mmu_asid_release(&proc->mm);
        mmu_free_ttbr(proc->mm.ttbr0);
        proc->mm.ttbr0 = 0;
        proc->mm.ttbr0_phys = 0;
    }
    destroy_fs(proc->permissions.fs_id);
    destroy_fs(proc->permissions.owned_fs_id);

    memset(proc->name, 0, sizeof(proc->name));

    proc->main_thread.stack_info = (stack_t){};

    proc->heap_phys = 0;
    memset(&proc->mm, 0, sizeof(proc->mm));

    proc->code = 0;
    proc->code_size = 0;
    proc->va = 0;
    proc->out_fd = (file){0};

    proc->win_id = 0;
    proc->win_fb_va = 0;
    proc->win_fb_phys = 0;
    proc->win_fb_size = 0;
    if (proc->bundle) release(proc->bundle);
    proc->bundle = 0;
    proc->focused = false;
    if (counted && proc_count) proc_count--;

    proc->id = pid;
    proc->exit_code = exit_code;
    proc->state = STOPPED;
}

void init_main_process(){
    size_t kernel_proc_size = (sizeof(process_t) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    kernel_proc = (process_t*)palloc(kernel_proc_size, MEM_PRIV_KERNEL, MEM_RW, true);
    if (!kernel_proc) panic("kernel process alloc failed", 0);
    idle_proc = (process_t*)palloc(kernel_proc_size, MEM_PRIV_KERNEL, MEM_RW, true);
    if (!idle_proc) panic("idle process alloc failed", 0);

    current_proc = kernel_proc;
    process_list = kernel_proc;
    cpec = (uintptr_t)kernel_proc;
    kernel_proc->id = next_proc_index++;
    kernel_proc->alloc_map = make_page_index();
    kernel_proc->state = BLOCKED;

    new_thread(kernel_proc, &kernel_proc->main_thread, 0x205, 0);
    
    kernel_proc->priority = PROC_PRIORITY_LOW;
    name_process(kernel_proc, "kernel");
    idle_proc->state = BLOCKED;
    idle_proc->priority = PROC_PRIORITY_LOW;

    new_thread(idle_proc, &idle_proc->main_thread, 0x205, (uptr)idle_entry);
    name_process(idle_proc, "idle");

    proc_count++;
}

process_t* init_process(){
    irq_flags_t irq = irq_save_disable();
    process_t* proc = process_list;
    while (proc) {
        if (proc != kernel_proc && proc->state == STOPPED && !proc->procfs_refs) {
            if (process_has_runtime_state(proc)) {
                irq_restore(irq);
                reset_process(proc);
                irq = irq_save_disable();
            }
            if (!process_has_runtime_state(proc)) {
                if (proc->postmortem_output) {
                    release((void*)proc->postmortem_output);
                    proc->postmortem_output = 0;
                    proc->postmortem_output_size = 0;
                }
                proc->id = next_proc_index++;
                proc->exit_code = 0;
                proc->state = BLOCKED;
                proc->priority = PROC_PRIORITY_LOW;
                proc->pending_reset = false;
                proc_count++;
                irq_restore(irq);
                return proc;
            }
        }
        proc = proc->process_next;
    }

    size_t proc_size = (sizeof(process_t) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    proc = palloc(proc_size, MEM_PRIV_KERNEL, MEM_RW, true);
    if (!proc) panic("Out of process memory", 0);

    proc->id = next_proc_index++;
    proc->state = BLOCKED;
    proc->priority = PROC_PRIORITY_LOW;
    proc->postmortem_output = 0;
    proc->postmortem_output_size = 0;
    proc->process_next = 0;
    if (!process_list) process_list= proc;
    else {
        process_t *tail = process_list;
        while (tail->process_next) tail = tail->process_next;
        tail->process_next = proc;
    }

    proc_count++;
    irq_restore(irq);
    return proc;
}

void name_process(process_t *proc, const char *name){
    if (!proc) return;

    memset(proc->name, 0, sizeof(proc->name));
    if (!name) return;

    uint32_t len = 0;
    while (len+1 < MAX_PROC_NAME_LENGTH && name[len] != '\0') {
        proc->name[len] = name[len];
        len++;
    }
}

void stop_process(uint16_t pid, int32_t exit_code){
    irq_flags_t irq = irq_save_disable();
    process_t *proc = get_proc_by_pid(pid);
    if (!proc || proc->state == STOPPED) {
        irq_restore(irq);
        return;
    }

    bool current = proc == current_proc;
    proc->state = STOPPED;
    proc->exit_code = exit_code;

    kprintf("[SCHEDULER] Stop process %i with code %i",proc->id,proc->exit_code);
    
    if (proc->focused)
        sys_unset_focus(false);
    else 
        window_close_process(proc);
    
    remove_sleeping_process(proc, pid);
    update_sleep_timer();
    if (!current) {
        irq_restore(irq);
        return;
    }

    //TODO: any threads for this process with a job id need to be reported back as failed

    if (proc->mm.ttbr0) mmu_swap_ttbr(0);
    switch_proc(HALT);
    panic("stop_process returned", pid);
}

void stop_current_process(int32_t exit_code){
    stop_process(get_current_proc_pid(), exit_code);
}

void block_process(process_t *proc){
    proc->suspended = true;
}

void resume_blocked_process(process_t *proc){
    proc->suspended = false;
    enqueue_ready_thread(&proc->main_thread);
}

uint16_t process_count(){
    return proc_count;
}

process_t *get_all_processes(){
    return process_list;
}

void sleep_thread(uint64_t msec){
    irq_flags_t irq = irq_save_disable();

    if (!msec) {
        switch_proc(YIELD);
        irq_restore(irq);
        return;
    }

    uint64_t wake_at = timer_now_msec() + msec;
    thread_t *current_thread = (thread_t*)cpec;
    current_thread->state = SLEEPING;
    current_thread->wake_at_msec = wake_at;

    linked_list_node_t *it = sleeping_list.head, *prev = 0;
    while (it) {
        thread_t *cur = (thread_t*)it->data;
        if (!cur || cur->wake_at_msec > wake_at) break;
        prev = it;
        it = it->next;
    }

    linked_list_insert_after(&sleeping_list, prev, current_thread);
    if (sleeping_list.head && sleeping_list.head->data == current_thread){
        virtual_timer_reset(msec);
        virtual_timer_enable();
    }
    switch_proc(YIELD);
    irq_restore(irq);
}

void wake_processes(){
    irq_flags_t irq = irq_save_disable();
    uint64_t now = timer_now_msec();
    while (sleeping_list.head) {
        thread_t *t = (thread_t*)sleeping_list.head->data;

        if (!t) {
            linked_list_pop_front(&sleeping_list);
            continue;
        }

        if (t->wake_at_msec > now) break;
        t = (thread_t*)linked_list_pop_front(&sleeping_list);

        if (t) {
            t->wake_at_msec = 0;

            if (t->state != STOPPED) enqueue_ready_thread(t);
        }
    }

    update_sleep_timer();
    irq_restore(irq);
}

void schedule_thread(process_t *proc, thread_t *t){
    enqueue_ready_thread(t);
}

thread_t* new_thread(process_t *proc, thread_t *addr, u64 spsr, uptr entry_point){
    if (addr != &proc->main_thread) spsr = proc->spsr;
    else proc->spsr = spsr;
    stack_t stack = new_stack(proc);
    if (!proc || !stack.top || !stack.size || !addr) return 0;
    *addr = (thread_t){
        .pc = entry_point,
        .pid = proc->id,
        .regs = {},
        .sp = stack.top,
        .stack_info = stack,
        .spsr = spsr,
        .tid = ++proc->thread_ids
        // .state = BLOCKED,
    };
    addr->regs[30] = is_privileged(proc) ? (uptr)kernel_thread_return_trampoline : proc->shared_page+sizeof(u32);
    return addr;
}

bool load_process_module(process_t *p, system_module *m, bool global){//TODO: this doesn't belong here
    if (!p->permissions.owned_fs_id) p->permissions.owned_fs_id = register_fs_id();
    module_root *root = get_fs_for_id(p->permissions.fs_id);
    system_module *mod = zalloc(sizeof(system_module));
    memcpy(mod, m, sizeof(system_module));
    mod->name = string_from_literal(m->name).data;
    mod->mount = string_from_literal(m->mount).data;
    mod->owner = p->id;
    bool ret = load_module_to(root, mod);
    if (!ret) return false;
    if (!global) return true;
    return load_module(mod);
}
