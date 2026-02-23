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
#include "sysregs.h"
#include "filesystem/filesystem.h"
#include "dev/module_loader.h"

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

extern void mmu_swap();

void switch_proc(ProcSwitchReason reason) {
    // kprintf("Stopping execution of process %i at %x",current_proc, processes[current_proc].spsr);
    if (proc_count == 0)
        panic("No processes active", 0);
    int next_proc = (current_proc + 1) % MAX_PROCS;
    while (processes[next_proc].state != READY) {
        next_proc = (next_proc + 1) % MAX_PROCS;
    }

    current_proc = next_proc;
    cpec = (uintptr_t)&processes[current_proc];
    timer_reset(processes[current_proc].priority);
    if (processes[current_proc].ttbr) mmu_asid_ensure(&processes[current_proc].asid, &processes[current_proc].asid_gen);
    mmu_swap_ttbr(processes[current_proc].ttbr, processes[current_proc].asid);
    process_restore();
}

void save_syscall_return(uint64_t value){
    processes[current_proc].PROC_X0 = value;
}

void process_restore(){
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
        proc_opened_files = chashmap_create(64);
        proc_opened_files->free = kfree;
        proc_opened_files->alloc = list_alloc;
    }
    return true;
}


uintptr_t get_current_heap(){
    return processes[current_proc].heap;
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

    if (proc->output) {
        pfree((void*)proc->output, PROC_OUT_BUF);
        proc->output = 0;
        proc->output_size = 0;
    }

    if (proc->id != 1 && proc->ttbr) {
        for (uint16_t i = 0; i < proc->mm.vma_count; i++) {
            vma *m = &proc->mm.vmas[i];
            for (uintptr_t va = m->start; va < m->end; va += GRANULE_4KB) {
                uint64_t pa = 0;
                if (!mmu_unmap_and_get_pa((uint64_t*)proc->ttbr, va, &pa)) continue;
                pfree((void*)pa, GRANULE_4KB);
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
        if (proc->id != 1) {
            for (page_index *ind = proc->alloc_map; ind; ind = ind->header.next) ind->header.size = 0;
        }
        release_page_index(proc->alloc_map);
        proc->alloc_map = 0;
    }
    if (proc->ttbr && proc->win_fb_size && proc->win_fb_phys) {
        for (uint64_t off = 0; off < proc->win_fb_size; off += GRANULE_4KB) mmu_unmap_table((uint64_t*)proc->ttbr, proc->win_fb_va + off, proc->win_fb_phys + off);
        proc->win_fb_va = 0;
        proc->win_fb_phys = 0;
        proc->win_fb_size = 0;
    }
    if (proc->ttbr) {
        if (pttbr == proc->ttbr) {
            kprintf("[PROC error] Trying to free process while mapped", (uintptr_t)proc->ttbr);
            return;
        }
        if (proc->asid) mmu_asid_release(proc->asid, proc->asid_gen);
        mmu_free_ttbr(proc->ttbr);
        proc->ttbr = 0;
    }
    proc->asid = 0;
    proc->asid_gen = 0;
    if (proc->exposed_fs.init){
        unload_module(&proc->exposed_fs);
    }
    proc->exposed_fs = (system_module){0};

    for (int k = 0; k < MAX_PROC_NAME_LENGTH; k++) proc->name[k] = 0;

    proc->stack = 0;
    proc->stack_phys = 0;
    proc->stack_size = 0;

    proc->heap = 0;
    proc->heap_phys = 0;

    proc->win_id = 0;

    if (!just_finished) {
        memset(proc, 0, sizeof(process_t));
        return;
    }

    proc->id = pid;
    proc->exit_code = exit_code;
    proc->state = state;
}

void init_main_process(){
    proc_page = palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    process_t* proc = &processes[0];
    cpec = (uintptr_t)&processes[0];
    proc->id = next_proc_index++;
    proc->alloc_map = make_page_index();
    proc->asid = 0;
    proc->asid_gen = 0;
    proc->state = BLOCKED;
    proc->heap = (uintptr_t)palloc(0x1000, MEM_PRIV_KERNEL, MEM_RW, false);
    proc->heap_phys = VIRT_TO_PHYS(proc->heap);
    proc->stack_size = 0x10000;
    proc->stack = (uintptr_t)palloc(proc->stack_size,MEM_PRIV_KERNEL, MEM_RW,true);
    proc->sp = (uintptr_t)ksp;
    proc->output = (uintptr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true);
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
                proc->asid = 0;
                proc->asid_gen = 0;
                proc_count++;
                proc->win_fb_va = 0;
                proc->win_fb_phys = 0;
                proc->win_fb_size = 0;
                return proc;
            }
        }
        panic("Out of process memory", 0);
    }

    proc = &processes[next_proc_index];
    reset_process(proc);
    for (int k = 0; k < MAX_PROC_NAME_LENGTH; k++) proc->name[k] = 0;
    proc->id = next_proc_index++;
    proc->asid = 0;
    proc->asid_gen = 0;
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
    if (proc->state != READY) return;
    proc->state = STOPPED;
    proc->exit_code = exit_code;
    if (proc->focused)
        sys_unset_focus();
    if (proc->ttbr) {
        mmu_swap_ttbr(0, 0);
        mmu_swap();
    }
    reset_process(proc);
    proc_count--;
    // kprintf("Stopped %i process %i",pid,proc_count);
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
            if(p) p->state = READY;
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
    p->exposed_fs.init = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.init - p->va));
    p->exposed_fs.fini = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.fini - p->va));
    p->exposed_fs.open = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.open - p->va));
    p->exposed_fs.read = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.read - p->va));
    p->exposed_fs.write = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.write - p->va));
    p->exposed_fs.close = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.close - p->va));
    p->exposed_fs.sread = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.sread - p->va));
    p->exposed_fs.swrite = PHYS_TO_VIRT_P(p->code + ((uintptr_t)p->exposed_fs.swrite - p->va));
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
    module_file *mfile = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(uint64_t));
    if (mfile){
        descriptor->id = mfile->fid;
        descriptor->size = mfile->file_size;
        mfile->references++;
        return FS_RESULT_SUCCESS;
    }
    const char *pid_s = seek_to(path, '/');
    path = seek_to(pid_s, '/');
    uint64_t pid = parse_int_u64(pid_s, path - pid_s);
    process_t *proc = get_proc_by_pid(pid);
    if (!proc) return FS_RESULT_NOTFOUND;
    descriptor->id = fid;
    descriptor->cursor = 0;
    module_file *file = kalloc(proc_page, sizeof(module_file), ALIGN_64B, MEM_PRIV_KERNEL);
    if (!file) return FS_RESULT_DRIVER_ERROR;
    memset(file, 0, sizeof(module_file));
    file->fid = fid;
    file->references = 1;
    if (strcmp_case(path, "out",true) == 0){
        if (!proc->output) proc->output = (uintptr_t)palloc(PROC_OUT_BUF, MEM_PRIV_KERNEL, MEM_RW, true);
        if (!proc->output) {
            kfree(file, sizeof(module_file));
            return FS_RESULT_DRIVER_ERROR;
        }
        descriptor->size = proc->output_size;
        file->file_buffer = (buffer){
            .buffer = (char*)proc->output,
            .buffer_size = proc->output_size,
            .limit = PROC_OUT_BUF,
            .options = buffer_circular,
            .cursor = proc->output_size,
        };
    } else if (strcmp_case(path, "state",true) == 0){
        descriptor->size = sizeof(int);
        file->read_only = true;
        file->file_buffer = (buffer){
            .buffer = (char*)PHYS_TO_VIRT((uintptr_t)&proc->state),
            .limit = sizeof(int),
            .options = buffer_static,
            .buffer_size = sizeof(int),
            .cursor = 0,
        };
    } else {
        kfree(file, sizeof(module_file));
        return FS_RESULT_NOTFOUND;
    }
    file->file_size = descriptor->size;
    return chashmap_put(proc_opened_files, &descriptor->id, sizeof(uint64_t), file) >= 0 ? FS_RESULT_SUCCESS : FS_RESULT_DRIVER_ERROR;
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
    module_file *file = (module_file*)chashmap_get(proc_opened_files, &fd->id, sizeof(uint64_t));
    if (!file) return 0;
    size_t s = buffer_read(&file->file_buffer, buf, size, offset);
    fd->size = file->file_size;
    return s;
}

size_t write_proc(file* fd, const char *buf, size_t size, file_offset offset){
    if (fd->id == FD_OUT){
        string fullpath = string_format("/%i/out",get_current_proc_pid());
        open_proc(fullpath.data, fd);
        string_free(fullpath);
    }
    if (!proc_opened_files){
        kprint("No files open");
        return 0;
    }
    module_file *file = (module_file*)chashmap_get(proc_opened_files, &fd->id, sizeof(uint64_t));
    if (!file) return 0;
    if (file->read_only) return 0;
    bool is_output = (uintptr_t)file->file_buffer.buffer == get_current_proc()->output;

    size = min(size, file->file_buffer.limit);
    if (is_output){//TODO: probably better to make these files be held by this module, and created only when needed
        size_t written= buffer_write_lim(&file->file_buffer, buf, size);
    
        file->file_size = file->file_buffer.buffer_size;
        get_current_proc()->output_size = file->file_size;
        fd->size = file->file_size;
        return written;
    }
    return 0;
}

void close_proc(file *fd) {
    if (!fd) return;
    if (!proc_opened_files) return;

    uint64_t fid = fd->id;
    module_file *mfile = (module_file*)chashmap_get(proc_opened_files, &fid, sizeof(fid));
    if (!mfile) return;

    if (mfile->references > 0) mfile->references--;
    if (mfile->references == 0) {
        chashmap_remove(proc_opened_files, &fid, sizeof(fid), 0);
        kfree(mfile, sizeof(module_file));
    }
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
