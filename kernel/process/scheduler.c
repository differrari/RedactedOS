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
#include "math/math.h"
#include "memory/mmu.h"
#include "process/syscall.h"
#include "memory/addr.h"
#include "sysregs.h"
#include "filesystem/filesystem.h"
#include "dev/module_loader.h"
#include "string/string.h"
#include "alloc/allocate.h"

extern void save_pc_interrupt(uintptr_t ptr);
extern void restore_context(uintptr_t ptr);

process_t *current_proc = 0;
process_t *kernel_proc = 0;
process_t *process_list = 0;
uint16_t proc_count = 0;
uint16_t next_proc_index = 1;

//TODO maybe use a weighted ready queue based on process priority
CQueue ready_queue = {};
linked_list_t sleeping_list = {};
process_t *reap_proc = 0;

chashmap_t *proc_opened_files;

void* proc_page;

void save_return_address_interrupt(){
    save_pc_interrupt(cpec);
}

void update_sleep_timer() {
    if (sleeping_list.head) {
        process_t *head_proc = (process_t*)sleeping_list.head->data;
        if (head_proc) {
            uint64_t now = timer_now_msec();
            uint64_t wait = head_proc->wake_at_msec > now ? head_proc->wake_at_msec - now : 1;
            virtual_timer_reset(wait);
            virtual_timer_enable();
        } else virtual_timer_disable();
    } else virtual_timer_disable();
}

void switch_proc(ProcSwitchReason reason) {
    // kprintf("Stopping execution of process %i at %x",current_proc, processes[current_proc].spsr);
    if (mmu_ttbr0_user_enabled()) panic("switch_proc with user ttbr0 active", current_proc ? current_proc->id : 0);
    if (proc_count == 0)
        panic("No processes active", 0);
    process_t*prev = current_proc, *next_proc = 0;
    if (prev && prev->state == RUNNING) ready_process(prev);
    while (!cqueue_is_empty(&ready_queue)) {
        process_t *queued = 0;
        if (!cqueue_dequeue(&ready_queue, &queued)) break;
        next_proc = queued;
        if (!next_proc) continue;
        next_proc->in_ready_queue = false;
        if (next_proc->state != READY) continue;
        if (next_proc->sleeping) continue;
        break;
    }

    if (!next_proc) {
        if (current_proc && current_proc->state == RUNNING) next_proc = current_proc;
        else panic("no runnable process", 0);
    }

    next_proc->state = RUNNING;
    current_proc = next_proc;
    cpec = (uintptr_t)current_proc;
    timer_reset(current_proc->priority);
    if (current_proc->mm.ttbr0) mmu_asid_ensure(&current_proc->mm);
    mmu_swap_ttbr(current_proc->mm.ttbr0 ? &current_proc->mm : 0);

    if (reap_proc &&reap_proc != current_proc && reap_proc != prev) {
        process_t *dead = reap_proc;
        reset_process(dead);
        reap_proc= 0;
    }

    process_restore();
}

void save_syscall_return(uint64_t value){
    current_proc->PROC_X0 = value;
}

void process_restore(){
    if ((current_proc->spsr & 0xF) == 0) {
        if (!current_proc->mm.ttbr0) panic("process_restore user process without ttbr0", current_proc->id);
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

void* list_alloc(size_t size){
    return kalloc(proc_page, size, ALIGN_64B, MEM_PRIV_KERNEL);
}

bool init_scheduler_module(){
    if (!proc_opened_files) {
        proc_opened_files = chashmap_create(1024);
        proc_opened_files->free = kfree;
        proc_opened_files->alloc = list_alloc;
    }
    if (!ready_queue.elem_size) cqueue_init(&ready_queue, 0, sizeof(process_t*),0,0);
    return true;
}


uintptr_t get_current_heap(){
    if (current_proc->heap_phys) return (uintptr_t)dmap_pa_to_kva(current_proc->heap_phys);
    return current_proc->mm.mmap_bottom;
}

bool get_current_privilege(){
    return current_proc && (current_proc->spsr & 0b1111) != 0;
}

process_t* get_current_proc(){
    return current_proc;
}

process_t* get_kernel_proc(){
    return kernel_proc;
}

void ready_process(process_t *proc){
    irq_flags_t irq = irq_save_disable();
    if (!proc || !proc->id || proc->state == STOPPED || proc->sleeping || proc->in_ready_queue) {
        irq_restore(irq);
        return;
    }

    if (!ready_queue.elem_size) cqueue_init(&ready_queue, 0, sizeof(process_t*),0,0);
    if (!cqueue_enqueue(&ready_queue, &proc)) panic("ready enqueue failed", proc->id);

    proc->in_ready_queue = true;
    proc->state = READY;
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

uint16_t get_current_proc_pid(){
    return current_proc ? current_proc->id : 0;
}

void reset_process(process_t *proc){
    uint16_t pid = proc->id;
    int32_t exit_code = proc->exit_code;
    uint8_t state = proc->state;
    bool counted = state != STOPPED;

    irq_flags_t irq = irq_save_disable();
    proc->sleeping = false;
    proc->wake_at_msec = 0;
    proc->in_ready_queue = false;

    linked_list_node_t *sleep = sleeping_list.head;
    while (sleep) {
        linked_list_node_t *next = sleep->next;
        process_t *sleep_proc = (process_t*)sleep->data;
        if (sleep_proc == proc || (sleep_proc && sleep_proc->id == pid)) {
            linked_list_remove(&sleeping_list, sleep);
        }
        sleep = next;
    }

    update_sleep_timer();
    irq_restore(irq);
    proc->sp = 0;
    proc->pc = 0;
    proc->spsr = 0;
    for (int j = 0; j < 31; j++)
        proc->regs[j] = 0;
    //for (int k = 0; k < MAX_PROC_NAME_LENGTH; k++)
        //proc->name[k] = 0;
    proc->input_buffer.read_index = 0;
    proc->input_buffer.write_index = 0;
    for (int k = 0; k < INPUT_BUFFER_CAPACITY; k++){
        proc->input_buffer.entries[k] = (keypress){0};
    }

    proc->event_buffer.read_index = 0;
    proc->event_buffer.write_index = 0;
    for (int k = 0; k < INPUT_BUFFER_CAPACITY; k++){
        proc->event_buffer.entries[k] = (kbd_event){0};
    }
    proc->packet_buffer.read_index = 0;
    proc->packet_buffer.write_index = 0;
    for (int k = 0; k < PACKET_BUFFER_CAPACITY; k++){
        sizedptr p = proc->packet_buffer.entries[k];
        if (p.ptr)
            free_sizedptr(p);
        proc->packet_buffer.entries[k] = (sizedptr){0};
    }
    close_files_for_process(pid);

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
        module_file *out_file = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(fid));
        if (out_file && (uintptr_t)out_file->file_buffer.buffer == (uintptr_t)proc->output) {
            size_t snapshot_size = proc->output_size;
            if (!snapshot_size) {
                out_file->file_buffer = (buffer){0};
                out_file->file_size = 0;
            } else {
                void *snapshot = kalloc(proc_page, snapshot_size, ALIGN_64B, MEM_PRIV_KERNEL);
                if (snapshot) {
                    memcpy(snapshot, (void*)proc->output, snapshot_size);
                    out_file->file_buffer = (buffer){
                        .buffer = snapshot,
                        .buffer_size = snapshot_size,
                        .limit = snapshot_size,
                        .options = buffer_opt_none,
                        .cursor = 0,
                    };
                    out_file->file_size = snapshot_size;
                } else {
                    out_file->file_buffer = (buffer){0};
                    out_file->file_size = 0;
                }
            }
        }

        string_format_buf(proc_path, sizeof(proc_path), "/%i/state", pid);
        fid = reserve_fd_gid(proc_path);
        module_file *state_file = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(fid));
        if (state_file && (uintptr_t)state_file->file_buffer.buffer == (uintptr_t)&proc->state) {
            uint8_t *snapshot = kalloc(proc_page, sizeof(state), ALIGN_64B, MEM_PRIV_KERNEL);
            if (snapshot) {
                *snapshot = state;
                state_file->file_buffer = (buffer){
                    .buffer = snapshot,
                    .buffer_size = sizeof(state),
                    .limit = sizeof(state),
                    .options = buffer_opt_none,
                    .cursor = 0,
                };
                state_file->file_size = sizeof(state);
            } else {
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
            for (uaddr_t va = m->start; va < m->end; va += GRANULE_4KB) {
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
    if (proc->exposed_fs.init){
        unload_module(&proc->exposed_fs);
    }
    proc->exposed_fs = (system_module){0};

    for (int k = 0; k < MAX_PROC_NAME_LENGTH; k++) proc->name[k] = 0;

    proc->stack = 0;
    proc->stack_phys = 0;
    proc->stack_size = 0;

    proc->heap_phys = 0;
    memset(&proc->mm, 0, sizeof(proc->mm));

    proc->win_id = 0;
    proc->win_fb_va = 0;
    proc->win_fb_phys = 0;
    proc->win_fb_size = 0;
    proc->bundle = 0;
    proc->focused = false;
    if (counted && proc_count) proc_count--;

    proc->id = pid;
    proc->exit_code = exit_code;
    proc->state = state;
}

void init_main_process(){
    proc_page = palloc(PAGE_SIZE*16, MEM_PRIV_KERNEL, MEM_RW, false);
    if (!ready_queue.elem_size) cqueue_init(&ready_queue, 0, sizeof(process_t*),0,0);
    size_t kernel_proc_size = (sizeof(process_t) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    kernel_proc = (process_t*)palloc(kernel_proc_size, MEM_PRIV_KERNEL, MEM_RW, true);
    if (!kernel_proc) panic("kernel process alloc failed", 0);

    memset(kernel_proc, 0, kernel_proc_size);
    current_proc = kernel_proc;
    process_list = kernel_proc;
    cpec = (uintptr_t)kernel_proc;
    kernel_proc->id = next_proc_index++;
    kernel_proc->alloc_map = make_page_index();
    kernel_proc->state = BLOCKED;
    kernel_proc->heap_phys = (uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    kernel_proc->stack_size = 0x10000;
    kernel_proc->stack = (uintptr_t)palloc(kernel_proc->stack_size,MEM_PRIV_KERNEL, MEM_RW,true);
    kernel_proc->sp = (uintptr_t)ksp;
    kernel_proc->output = (kaddr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true);
    kernel_proc->output_size = 0;
    kernel_proc->priority = PROC_PRIORITY_LOW;
    name_process(kernel_proc, "kernel");
    proc_count++;
}

process_t* init_process(){
    process_t* proc = kalloc(proc_page, sizeof(process_t), ALIGN_64B, MEM_PRIV_KERNEL);
    if (!proc) panic("Out of process memory", 0);

    memset(proc, 0, sizeof(process_t));
    proc->id = next_proc_index++;
    proc->state = BLOCKED;
    proc->priority = PROC_PRIORITY_LOW;
    if (!process_list) process_list= proc;
    else {
        process_t *tail = process_list;
        while (tail->process_next) tail = tail->process_next;
        tail->process_next = proc;
    }

    proc_count++;
    return proc;
}

void name_process(process_t *proc, const char *name){
    uint32_t len = 0;
    while (len < MAX_PROC_NAME_LENGTH && name[len] != '\0') len++;
    for (uint32_t i = 0; i < len; i++)
        proc->name[i] = name[i];
}

void stop_process(uint16_t pid, int32_t exit_code){
    disable_interrupt();
    process_t *proc = get_proc_by_pid(pid);
    if (!proc || proc->state == STOPPED) {
        enable_interrupt();
        return;
    }

    bool current = proc == current_proc;
    proc->state = STOPPED;
    proc->exit_code = exit_code;
    proc->in_ready_queue = false;
    proc->sleeping = false;
    proc->wake_at_msec = 0;
    if (proc->focused)
        sys_unset_focus();
    //if (current && proc->mm.ttbr0) mmu_swap_ttbr(0);
    //reset_process(proc);
    // kprintf("Stopped %i process %i",pid,proc_count);
    if (!current) {
        reset_process(proc);
        enable_interrupt();
        return;
    }

    linked_list_node_t *sleep = sleeping_list.head;
    while (sleep) {
        linked_list_node_t *next = sleep->next;
        process_t *sleep_proc = (process_t*)sleep->data;
        if (sleep_proc == proc || (sleep_proc && sleep_proc->id == pid)) {
            linked_list_remove(&sleeping_list, sleep);
        }
        sleep = next;
    }

    update_sleep_timer();

    if (proc->mm.ttbr0) mmu_swap_ttbr(0);
    reap_proc = proc;
    switch_proc(HALT);
}

void stop_current_process(int32_t exit_code){
    disable_interrupt();
    stop_process(get_current_proc_pid(), exit_code);
}

uint16_t process_count(){
    return proc_count;
}

process_t *get_all_processes(){
    return process_list;
}

void sleep_process(uint64_t msec){
    irq_flags_t irq = irq_save_disable();

    if (!msec) {
        switch_proc(YIELD);
        irq_restore(irq);
        return;
    }

    uint64_t wake_at = timer_now_msec() + msec;
    current_proc->state = BLOCKED;
    current_proc->sleeping = true;
    current_proc->wake_at_msec = wake_at;

    linked_list_node_t *it = sleeping_list.head, *prev = 0;
    while (it) {
        process_t *cur = (process_t*)it->data;
        if (!cur || cur->wake_at_msec > wake_at) break;
        prev = it;
        it = it->next;
    }

    linked_list_insert_after(&sleeping_list, prev, current_proc);
    if (sleeping_list.head && sleeping_list.head->data == current_proc){
        virtual_timer_reset(msec);
        virtual_timer_enable();
    }
    switch_proc(YIELD);
    irq_restore(irq);
}

void wake_process(process_t *proc){
    if (!proc) return;
    irq_flags_t irq = irq_save_disable();

    if (proc->state == STOPPED) {
        irq_restore(irq);
        return;
    }

    linked_list_node_t *node = sleeping_list.head;
    while (node) {
        linked_list_node_t *next = node->next;
        process_t *sleep_proc = (process_t*)node->data;
        if (sleep_proc == proc) {
            linked_list_remove(&sleeping_list, node);

            proc->sleeping = false;
            proc->wake_at_msec = 0;

            if (proc->state == BLOCKED) {
                if (!proc->in_ready_queue) {
                    if (!ready_queue.elem_size) cqueue_init(&ready_queue,0, sizeof(process_t*),0,0);
                    if (!cqueue_enqueue(&ready_queue, &proc)) panic("ready enqueue failed", proc->id);
                    proc->in_ready_queue = true;
                }
                proc->state = READY;
            }

            break;
        }

        node = next;
    }

    update_sleep_timer();
    irq_restore(irq);
}

void wake_processes(){
    irq_flags_t irq = irq_save_disable();
    uint64_t now = timer_now_msec();
    while (sleeping_list.head) {
        process_t *proc = (process_t*)sleeping_list.head->data;

        if (!proc) {
            linked_list_pop_front(&sleeping_list);
            continue;
        }

        if (proc->wake_at_msec > now) break;
        proc = (process_t*)linked_list_pop_front(&sleeping_list);

        if (proc) {
            proc->sleeping = false;
            proc->wake_at_msec = 0;

            if (proc->state != STOPPED) {
                if (!proc->in_ready_queue) {
                    if (!ready_queue.elem_size) cqueue_init(&ready_queue,0, sizeof(process_t*),0,0);
                    if (!cqueue_enqueue(&ready_queue, &proc)) panic("ready enqueue failed", proc->id);
                    proc->in_ready_queue = true;
                }
                proc->state = READY;
            }
        }
    }

    update_sleep_timer();
    irq_restore(irq);
}

bool load_process_module(process_t *p, system_module *m){
    p->exposed_fs = *m;
    p->exposed_fs.init = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.init - (uintptr_t)p->va));
    p->exposed_fs.fini = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.fini - (uintptr_t)p->va));
    p->exposed_fs.open = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.open - (uintptr_t)p->va));
    p->exposed_fs.read = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.read - (uintptr_t)p->va));
    p->exposed_fs.write = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.write - (uintptr_t)p->va));
    p->exposed_fs.close = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.close - (uintptr_t)p->va));
    p->exposed_fs.sread = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.sread - (uintptr_t)p->va));
    p->exposed_fs.swrite = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.swrite - (uintptr_t)p->va));
    return load_module(&p->exposed_fs);
}

size_t list_processes(const char *path, void *buf, size_t size, file_offset *offset){

	uint32_t count = 0;
	
    char *write_ptr = (char*)buf + 4;
    process_t *proc = process_list;
    if (*offset) {
        while (proc && proc->id != *offset) proc = proc->process_next;
        if (proc) proc = proc->process_next;
    }

    while (proc) {
    	if (size - (uintptr_t)write_ptr - (uintptr_t)buf - 4 < MAX_PROC_NAME_LENGTH) break;
        if (proc->id != 0 && proc->state != STOPPED){
            count++;
            char* name = proc->name;
            while (*name) *write_ptr++ = *name++;
            *write_ptr++ = 0;
            *offset = proc->id;
        }
        proc = proc->process_next;
    }

    *(uint32_t*)buf = count;

    //TODO: allow seeing files belonging to a proc (/out, /in, etc)
    return size;
}

FS_RESULT open_proc(const char *path, file *descriptor){
    uint64_t fid = reserve_fd_gid(path);
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(uint64_t));
    if (mfile){
        descriptor->id = mfile->fid;
        descriptor->size = mfile->file_size;
        descriptor->cursor = 0;
        mfile->references++;
        irq_restore(irq);
        return FS_RESULT_SUCCESS;
    }
    const char *pid_s = seek_to(path, '/');
    path = seek_to(pid_s, '/');
    uint64_t pid = parse_int_u64(pid_s, path - pid_s);
    process_t *proc = get_proc_by_pid(pid);
    if (!proc) {
        irq_restore(irq);
        return FS_RESULT_NOTFOUND;
    }
    descriptor->id = fid;
    descriptor->cursor = 0;
    module_file *file = kalloc(proc_page, sizeof(module_file), ALIGN_64B, MEM_PRIV_KERNEL);
    if (!file) {
        irq_restore(irq);
        return FS_RESULT_DRIVER_ERROR;
    }
    memset(file, 0, sizeof(module_file));
    file->fid = fid;
    file->references = 1;
    if (strcmp_case(path, "out",true) == 0){
        descriptor->size = proc->output_size;
        file->read_only = true;
        file->file_buffer = (buffer){
            .buffer = (char*)proc->output,
            .buffer_size = proc->output ? proc->output_size : 0,
            .limit = proc->output ? PROC_OUT_BUF : 0,
            .options = proc->output ? buffer_circular : buffer_opt_none,
            .cursor = proc->output ? proc->output_size : 0,
        };
    } else if (strcmp_case(path, "state",true) == 0){
        descriptor->size = sizeof(proc->state);
        file->read_only = true;
        file->file_buffer = (buffer){
            .buffer = (char*)&proc->state,
            .limit = sizeof(proc->state),
            .options = buffer_static,
            .buffer_size = sizeof(proc->state),
            .cursor = 0,
        };
    } else {
        irq_restore(irq);
        kfree(file, sizeof(module_file));
        return FS_RESULT_NOTFOUND;
    }
    file->file_size = descriptor->size;
    int put = chashmap_put(proc_opened_files, &descriptor->id, sizeof(uint64_t), file);
    irq_restore(irq);
    if (put >= 0) return FS_RESULT_SUCCESS;
    kfree(file, sizeof(module_file));
    return FS_RESULT_DRIVER_ERROR;
}

int find_open_proc_file(void *node, void* key){
    uint64_t *fid = (uint64_t*)key;
    module_file *file = (module_file*)node;
    if (file->fid == *fid) return 0;
    return -1;
}

int find_open_proc_file_buffer(void *node, void* key){
    uintptr_t *buf = (uintptr_t*)key;
    module_file *file = (module_file*)node;
    if ((uintptr_t)file->file_buffer.buffer == *buf) return 0;
    return -1;
}

size_t read_proc(file* fd, char *buf, size_t size, file_offset offset){
    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    irq_flags_t irq = irq_save_disable();
    module_file *file = (module_file*)chashmap_get(proc_opened_files, &fd->id, sizeof(uint64_t));
    if (!file) {
        irq_restore(irq);
        return 0;
    }
    size_t s = buffer_read(&file->file_buffer, buf, size, offset);
    fd->size = file->file_size;
    irq_restore(irq);
    return s;
}

size_t write_proc(file* fd, const char *buf, size_t size, file_offset offset){
    process_t *proc = get_current_proc();
    if (fd->id == FD_OUT){
        if (!proc || !proc->output || !size) return 0;
        irq_flags_t irq = irq_save_disable();

        buffer file_buffer = {
            .buffer = (char*)proc->output,
            .buffer_size = proc->output_size,
            .limit = PROC_OUT_BUF,
            .options = buffer_circular,
            .cursor = proc->output_size,
        };

        size = min(size, file_buffer.limit);
        size_t written = buffer_write_lim(&file_buffer, buf, size);

        proc->output_size = file_buffer.buffer_size;
        fd->size = proc->output_size;

        if (proc_opened_files){
            char fullpath[48] = {};
            string_format_buf(fullpath, sizeof(fullpath), "/%i/out", proc->id);
            uint64_t fid = reserve_fd_gid(fullpath);
            module_file *file = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(fid));
            if (file && (uintptr_t)file->file_buffer.buffer == (uintptr_t)proc->output) {
                file->file_buffer.buffer_size = proc->output_size;
                file->file_buffer.limit = PROC_OUT_BUF;
                file->file_buffer.cursor = proc->output_size;
                file->file_buffer.options = buffer_circular;
                file->file_size = proc->output_size;
            }
        }

        irq_restore(irq);
        return written;
    }

    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    irq_flags_t irq = irq_save_disable();
    module_file *file = (module_file*)chashmap_get(proc_opened_files, &fd->id, sizeof(uint64_t));
    bool ro = file && file->read_only;
    irq_restore(irq);
    if (!file) return 0;
    if (ro) return 0;
    return 0;
}

void close_proc(file *fd) {
    if (!fd) return;
    if (!proc_opened_files) return;

    uint64_t fid = fd->id;
    irq_flags_t irq = irq_save_disable();
    module_file *mfile = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(fid));
    if (!mfile) {
        irq_restore(irq);
        return;
    }

    if (mfile->references > 0) mfile->references--;
    if (mfile->references == 0) {
        void *owned = mfile->file_buffer.buffer;
        size_t owned_size = mfile->file_size;
        buffer_options options = mfile->file_buffer.options;
        chashmap_remove(proc_opened_files, &fid, sizeof(fid), 0);
        irq_restore(irq);
        if (owned && options == buffer_opt_none) kfree(owned, owned_size ? owned_size : 1);
        kfree(mfile, sizeof(module_file));
        return;
    }
    irq_restore(irq);
}

system_module scheduler_module = (system_module){
    .name = "scheduler",
    .mount = "/proc",
    .version = VERSION_NUM(0, 1, 0, 1),
    .init = init_scheduler_module,
    .fini = 0,
    .open = open_proc,
    .read = read_proc,
    .write = write_proc,
    .close = close_proc,
    .sread = 0,
    .swrite = 0,//TODO implement simple io
    .readdir = list_processes,
};
