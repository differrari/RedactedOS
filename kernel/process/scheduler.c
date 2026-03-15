#include "scheduler.h"
#include "console/kio.h"
#include "memory/page_allocator.h"
#include "exceptions/irq.h"
#include "input/input_dispatch.h"
#include "exceptions/exception_handler.h"
#include "exceptions/timer.h"
#include "console/kconsole/kconsole.h"
#include "data/struct/hashmap.h"
#include "std/memory.h"
#include "math/math.h"
#include "memory/mmu.h"
#include "process/syscall.h"
#include "memory/addr.h"
#include "sysregs.h"
#include "filesystem/filesystem.h"
#include "dev/module_loader.h"
#include "string/string.h"

extern void save_pc_interrupt(uintptr_t ptr);
extern void restore_context(uintptr_t ptr);

//TODO: use queues, eliminate the max procs limitation
process_t processes[MAX_PROCS];
uint16_t current_proc = 0;
uint16_t proc_count = 0;
uint16_t next_proc_index = 1;

typedef struct sleep_tracker {
    uint16_t pid;
    uint64_t timestamp;
    uint64_t sleep_time;
    bool valid;
} sleep_tracker;

sleep_tracker sleeping[MAX_PROCS];
uint16_t sleep_count;

chashmap_t *proc_opened_files;

void* proc_page;

void save_return_address_interrupt(){
    save_pc_interrupt(cpec);
}


void switch_proc(ProcSwitchReason reason) {
    // kprintf("Stopping execution of process %i at %x",current_proc, processes[current_proc].spsr);
    if (mmu_ttbr0_user_enabled()) panic("switch_proc with user ttbr0 active", current_proc);
    if (proc_count == 0)
        panic("No processes active", 0);
    int next_proc = (current_proc + 1) % MAX_PROCS;
    while (processes[next_proc].state != READY) {
        next_proc = (next_proc + 1) % MAX_PROCS;
    }

    current_proc = next_proc;
    cpec = (uintptr_t)&processes[current_proc];
    timer_reset(processes[current_proc].priority);
    if (processes[current_proc].mm.ttbr0) mmu_asid_ensure(&processes[current_proc].mm);
    mmu_swap_ttbr(processes[current_proc].mm.ttbr0 ? &processes[current_proc].mm : 0);
    process_restore();
}

void save_syscall_return(uint64_t value){
    processes[current_proc].PROC_X0 = value;
}

void process_restore(){
    if ((processes[current_proc].spsr & 0xF) == 0) {
        if (!processes[current_proc].mm.ttbr0) panic("process_restore user process without ttbr0", processes[current_proc].id);
        mmu_ttbr0_enable_user();
    } else mmu_ttbr0_disable_user();
    restore_context(cpec);
}

bool start_scheduler(){
    kprint("Starting scheduler");
    kconsole_clear();
    disable_interrupt();
    timer_init(processes[current_proc].priority);
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
    return true;
}


uintptr_t get_current_heap(){
    if (processes[current_proc].mm.ttbr0) return processes[current_proc].mm.brk;
    return processes[current_proc].mm.brk;
}

bool get_current_privilege(){
    return (processes[current_proc].spsr & 0b1111) != 0;
}

process_t* get_current_proc(){
    return &processes[current_proc];
}

process_t* get_proc_by_pid(uint16_t pid){
    for (int i = 0; i < MAX_PROCS; i++)
        if (processes[i].id == pid)
            return &processes[i];
    return NULL;
}

uint16_t get_current_proc_pid(){
    return processes[current_proc].id;
}

void reset_process(process_t *proc){
    uint16_t pid = proc->id;
    int32_t exit_code = proc->exit_code;
    uint8_t state = proc->state;
    bool counted = state != STOPPED;
    for (uint16_t i = 0; i < sleep_count; i++) {
        if (sleeping[i].pid != pid) continue;
        sleeping[i].valid = false;
    }

    bool just_finished = processes[current_proc].id == pid;
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
        irq_flags_t irq = irq_save_disable();
        char proc_path[48] = {};
        string_format_buf(proc_path, sizeof(proc_path), "/%i/out", pid);
        uint64_t fid = reserve_fd_gid(proc_path);
        module_file *out_file = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(fid));
        if (out_file && (uintptr_t)out_file->file_buffer.buffer == (uintptr_t)proc->output) {
            size_t snapshot_size = proc->output_size;
            out_file->buf = 0;
            out_file->owns_buf = false;
            out_file->buf_is_page_alloc = false;
            if (!snapshot_size) {
                out_file->file_buffer = (buffer){0};
                out_file->file_size = 0;
            } else {
                out_file->file_buffer = (buffer){
                    .buffer = (char*)proc->output,
                    .buffer_size = snapshot_size,
                    .limit = snapshot_size,
                    .options = buffer_opt_none,
                    .cursor = 0,
                };
                out_file->buf = (uintptr_t)proc->output;
                out_file->owns_buf = true;
                out_file->buf_is_page_alloc = true;
                out_file->file_size = snapshot_size;
                proc->output = 0;
                proc->output_size = 0;
            }
        }

        string_format_buf(proc_path, sizeof(proc_path), "/%i/state", pid);
        fid = reserve_fd_gid(proc_path);
        module_file *state_file = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(fid));
        if (state_file && (uintptr_t)state_file->file_buffer.buffer == (uintptr_t)&proc->state) {
            memset(&state_file->buf, 0, sizeof(state_file->buf));
            memcpy(&state_file->buf, &state, sizeof(state));
            state_file->owns_buf = false;
            state_file->buf_is_page_alloc = false;
            state_file->file_buffer = (buffer){
                .buffer = (char*)&state_file->buf,
                .buffer_size = sizeof(state),
                .limit = sizeof(state),
                .options = buffer_static,
                .cursor = 0,
            };
            state_file->file_size = sizeof(state);
        }
        irq_restore(irq);
    }

    if (proc->output && !just_finished) {
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
                if (m->kind == VMA_KIND_HEAP) {
                    if (proc->mm.rss_heap_pages) proc->mm.rss_heap_pages--;
                } else if (m->kind == VMA_KIND_STACK) {
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
    if (counted && proc_count) proc_count--;

    if (!just_finished) {
        memset(proc, 0, sizeof(process_t));
        return;
    }

    proc->id = pid;
    proc->exit_code = exit_code;
    proc->state = state;
}

void init_main_process(){
    proc_page = palloc(PAGE_SIZE*16, MEM_PRIV_KERNEL, MEM_RW, false);
    process_t* proc = &processes[0];
    cpec = (uintptr_t)&processes[0];
    proc->id = next_proc_index++;
    proc->alloc_map = make_page_index();
    proc->state = BLOCKED;
    proc->heap_phys = (uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    proc->mm.heap_start = (uaddr_t)dmap_pa_to_kva(proc->heap_phys);
    proc->mm.brk = proc->mm.heap_start;
    proc->stack_size = 0x10000;
    proc->stack = (uintptr_t)palloc(proc->stack_size,MEM_PRIV_KERNEL, MEM_RW,true);
    proc->sp = (uintptr_t)ksp;
    proc->output = (kaddr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true);
    proc->output_size = 0;
    proc->priority = PROC_PRIORITY_LOW;
    proc->win_fb_va = 0;
    proc->win_fb_phys = 0;
    proc->win_fb_size = 0;
    name_process(proc, "kernel");
    proc_count++;
}

process_t* init_process(){
    process_t* proc;
    if (next_proc_index >= MAX_PROCS){
        for (uint16_t i = 0; i < MAX_PROCS; i++){
            if (processes[i].state == STOPPED){
                proc = &processes[i];
                reset_process(proc);
                for (int k = 0; k < MAX_PROC_NAME_LENGTH; k++) proc->name[k] = 0;
                proc->state = READY;
                proc->id = next_proc_index++;
                proc->priority = PROC_PRIORITY_LOW;
                proc->win_fb_va = 0;
                proc->win_fb_phys = 0;
                proc->win_fb_size = 0;
                proc_count++;
                return proc;
            }
        }
        panic("Out of process memory", 0);
    }

    proc = &processes[next_proc_index];
    reset_process(proc);
    for (int k = 0; k < MAX_PROC_NAME_LENGTH; k++) proc->name[k] = 0;
    proc->id = next_proc_index++;
    proc->state = READY;
    proc->priority = PROC_PRIORITY_LOW;
    proc->win_fb_va = 0;
    proc->win_fb_phys = 0;
    proc->win_fb_size = 0;
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

    bool current = proc == &processes[current_proc];
    proc->state = STOPPED;
    proc->exit_code = exit_code;
    if (proc->focused)
        sys_unset_focus();
    if (current && proc->mm.ttbr0) mmu_swap_ttbr(0);
    reset_process(proc);
    // kprintf("Stopped %i process %i",pid,proc_count);
    if (!current) {
        enable_interrupt();
        return;
    }
    switch_proc(HALT);
}

void stop_current_process(int32_t exit_code){
    disable_interrupt();
    stop_process(processes[current_proc].id, exit_code);
}

uint16_t process_count(){
    return proc_count;
}

process_t *get_all_processes(){
    return processes;
}

void sleep_process(uint64_t msec){
    if (!msec) switch_proc(YIELD);
    if (sleep_count < MAX_PROCS){
        processes[current_proc].state = BLOCKED;
        sleeping[sleep_count++] = (sleep_tracker){
            .pid = processes[current_proc].id,
            .timestamp = timer_now_msec(),
            .sleep_time = msec, 
            .valid = true
        };
    }
    if (virtual_timer_remaining_msec() > msec || virtual_timer_remaining_msec() == 0){
        virtual_timer_reset(msec);
        virtual_timer_enable();
    }
    switch_proc(YIELD);
}

void wake_processes(){
    uint64_t now = timer_now_msec();
    uint64_t next = UINT64_MAX;
    uint16_t w = 0;
    for(uint16_t i=0;i<sleep_count;i++){
        if(!sleeping[i].valid) continue;
        uint64_t wake = sleeping[i].timestamp + sleeping[i].sleep_time;
        if(wake <= now){
            process_t *p = get_proc_by_pid(sleeping[i].pid);
            if(p && p->state == BLOCKED) p->state = READY;
        }else{
            if(wake < next) next = wake;
            sleeping[w++] = sleeping[i];
        }
    }
    sleep_count = w;

    if(next != UINT64_MAX){
        virtual_timer_reset(next - now);
        virtual_timer_enable();
    }
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
    
   	int proc_index = 0;
    if (*offset){
        for (int i = 0; i < MAX_PROCS; i++){
            if (processes[i].id == *offset){
                proc_index = i+1;
                break;
            }
        }
    }

	uint32_t count = 0;
	
    char *write_ptr = (char*)buf + 4;
    for (; proc_index < MAX_PROCS; proc_index++){
    	if (size - (uintptr_t)write_ptr - (uintptr_t)buf - 4 < MAX_PROC_NAME_LENGTH) break;
   		process_t *proc = &processes[proc_index];
        if (proc->id != 0 && proc->state != STOPPED){
            count++;
            char* name = proc->name;
            while (*name) *write_ptr++ = *name++;
            *write_ptr++ = 0;
            *offset = proc->id;
        }
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
        if (!proc->output || !size) return 0;
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
            if (file && (uintptr_t)file->file_buffer.buffer == (uintptr_t)proc->output && !file->owns_buf) {
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
        if (mfile->owns_buf && mfile->buf && mfile->file_buffer.buffer) {
            if (mfile->buf_is_page_alloc) pfree((void*)mfile->buf, PROC_OUT_BUF);
            else kfree((void*)mfile->buf, mfile->file_size);
            mfile->buf = 0;
            mfile->owns_buf = false;
            mfile->buf_is_page_alloc = false;
        }
        chashmap_remove(proc_opened_files, &fid, sizeof(fid), 0);
        irq_restore(irq);
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
